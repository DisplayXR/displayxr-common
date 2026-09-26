// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The Wayland drag lattice's table build, at ANY output scale
 *         (runtime#1609 follow-up). Pure: no window, bus or graphics state, so
 *         it runs on a worker thread and is unit tested without a compositor.
 *
 * ## Why this is not "every q-th device pixel"
 *
 * The compositor places windows at integer LOGICAL positions. What reaches the
 * panel is where Mutter DRAWS the surface, and Mutter snaps that to the
 * physical pixel grid itself (mutter 50,
 * `src/compositor/meta-window-actor-wayland.c`,
 * `surface_container_apply_transform`):
 *
 *     device = roundf((surface_logical - monitor_logical) * scale)
 *
 * with `monitor` and `scale` taken from the highest-scale monitor the window
 * is on, and `roundf` rounding halves away from zero. The same formula is
 * used by the runtime's geometry consumer (`u_wl_logical_to_px`), so the
 * position the lattice reasons about is the position the weave is phased to.
 *
 * At an integer scale q that is `q * logical` and the reachable device
 * positions are every q-th pixel. At 1.5 they are 0, 2, 3, 5, 6, 8, … (3
 * device pixels per 2 logical); at 1.6667 0, 2, 3, 5, 7, 8, … — no period
 * short enough to call a lattice, but still a KNOWN set. So the table is built
 * over LOGICAL displacements: for each candidate, the device displacement it
 * actually produces is computed with the formula above, and the display
 * processor's snap (the only oracle — nothing is derived from lens geometry)
 * says whether that device displacement is phase-correct. The device
 * displacement of a logical move depends on where the move starts (the
 * rounding), so the table is built for one known start: `rel0`, the surface's
 * logical position relative to the monitor at the drag's start.
 *
 * At an integer scale the result is identical to the integer-only build it
 * replaces (tests/linux_window_lattice_test.cpp pins that).
 */
#pragma once

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <utility>
#include <vector>

namespace dxr_wl_lattice {

//! Mutter's surface placement: device px = round-half-away(rel_logical * scale).
inline int32_t
logical_to_px(int32_t rel_logical, double scale)
{
	const double v = (double)rel_logical * scale;
	return (int32_t)(v >= 0.0 ? v + 0.5 : v - 0.5);
}

//! Where a table is built from: the start, in the monitor's frame.
struct Map
{
	int32_t rel0_x = 0, rel0_y = 0; //!< surface logical position - monitor logical origin, at the start
	double scale = 1.0;             //!< the monitor's scale (Mutter's, as the geometry service publishes it)
};

//! Device displacement a LOGICAL displacement from the start produces.
inline void
device_displacement(const Map &m, int32_t dx, int32_t dy, int32_t *out_x, int32_t *out_y)
{
	*out_x = logical_to_px(m.rel0_x + dx, m.scale) - logical_to_px(m.rel0_x, m.scale);
	*out_y = logical_to_px(m.rel0_y + dy, m.scale) - logical_to_px(m.rel0_y, m.scale);
}

//! A logical displacement whose device displacement is closest to (px, py).
inline void
nearest_logical(const Map &m, int32_t px, int32_t py, int32_t *out_x, int32_t *out_y)
{
	*out_x = (int32_t)std::lround((double)px / m.scale);
	*out_y = (int32_t)std::lround((double)py / m.scale);
}

struct Probe
{
	std::vector<int32_t> dxs, dys; //!< phase-correct LOGICAL displacements
	size_t probed = 0, fixed = 0;
	bool declined = false;

	//! @name Bulk path (probe_via_grid) — all zero for the per-point probe.
	//! @{
	bool grid_used = false;     //!< the table was answered from grid calls
	uint32_t grid_calls = 0;    //!< grid provider calls made for the table
	uint64_t grid_points = 0;   //!< points those calls returned
	uint32_t grid_misses = 0;   //!< snap queries the fetched grids did not cover (asked singly)
	const char *grid_fallback = nullptr; //!< why the per-point probe ran instead, if it did
	//! @}
};

/*!
 * Probe the display processor over a grid of LOGICAL displacements centred on
 * (@p cx, @p cy), @p half either side, every @p cell. For each cell: ask the
 * snap where the nearest phase-correct DEVICE displacement is, then find a
 * LOGICAL displacement that produces exactly that device displacement; when
 * none does, search the logical neighbourhood for one whose device
 * displacement the snap leaves unchanged. One entry per cell at most.
 *
 * @p snap is `bool(int32_t tx, int32_t ty, int32_t *ox, int32_t *oy)`: the
 * DP's snap with origin (0,0), target = a DEVICE displacement; false = the DP
 * declined (no usable viewing distance).
 *
 * ## @p cell == 1: the dense table (runtime#1748)
 *
 * A coarser @p cell keeps at most one entry per cell, so the table misses
 * reachable phase-correct points the lens has, and the window is pulled
 * further than it needs to be. Measured with a snap of the vendor weaver's
 * shape (best phase within ±2 device px) over a representative slanted lens,
 * a 3 px cell leaves 18 % of those points out at 200 % and 31 % at 150 %; the
 * largest pull is ~6.3 device px where the complete set needs ~4.5, and a
 * slow straight drag wiggles across its direction by that much.
 *
 * At @p cell 1 every logical displacement in the window is a query, so each
 * reachable point the snap leaves where it is (a fixed point) is found as its
 * own query's answer, and every reachable answer is kept: no entry the coarse
 * table has inside the window is missing. The fixed-point search around an
 * unreachable answer is therefore skipped (it only finds points the dense
 * sweep visits anyway); what remains is a mapping-only check of the rounded
 * guess's neighbours for a logical displacement that reaches the answer
 * exactly. Duplicates are removed across the whole table rather than among
 * the last few entries, since one reachable point answers several
 * neighbouring queries. The table stays the size of the coarse one (~13k
 * entries for ±192 logical px), and the probe's own CPU stays ~10 ms.
 */
template <typename Snap>
inline Probe
probe(const Snap &snap, const Map &m, int32_t cx, int32_t cy, int32_t half, int32_t cell)
{
	Probe r;
	const bool dense = cell == 1;
	std::vector<std::pair<int32_t, int32_t>> seen;
	// Dense: one bit per logical displacement of the window (plus the few px
	// an answer can sit outside it), for the whole-table dedup.
	const int32_t dpad = 8;
	const int32_t dspan = dense ? 2 * (half + dpad) + 1 : 0;
	std::vector<uint8_t> taken(dense ? (size_t)dspan * dspan : 0, 0);
	auto fixed_point = [&](int32_t lx, int32_t ly) {
		int32_t tx = 0, ty = 0;
		device_displacement(m, lx, ly, &tx, &ty);
		int32_t ox = tx, oy = ty;
		return snap(tx, ty, &ox, &oy) && ox == tx && oy == ty;
	};

	for (int32_t gy = cy - half; gy <= cy + half && !r.declined; gy += cell) {
		for (int32_t gx = cx - half; gx <= cx + half; gx += cell) {
			r.probed++;
			int32_t tx = 0, ty = 0;
			device_displacement(m, gx, gy, &tx, &ty);
			int32_t sx = tx, sy = ty;
			if (!snap(tx, ty, &sx, &sy)) {
				r.declined = true;
				break;
			}
			if (sx == tx && sy == ty) {
				r.fixed++;
			}
			// The DP's answer as a logical displacement, if one reaches it
			// exactly; else the nearest reachable one the DP also accepts.
			int32_t bx = 0, by = 0;
			nearest_logical(m, sx, sy, &bx, &by);
			int32_t ax = 0, ay = 0;
			bool found = false;
			{
				int32_t px = 0, py = 0;
				device_displacement(m, bx, by, &px, &py);
				if (px == sx && py == sy) {
					ax = bx;
					ay = by;
					found = true;
				}
			}
			// Dense: the rounded guess can miss a logical displacement that
			// DOES reach the answer (the rounding depends on the start), so
			// look at its neighbours — no snap call, only the mapping.
			for (int32_t j = -1; j <= 1 && dense && !found; j++) {
				for (int32_t i = -1; i <= 1 && !found; i++) {
					int32_t px = 0, py = 0;
					device_displacement(m, bx + i, by + j, &px, &py);
					if (px == sx && py == sy) {
						ax = bx + i;
						ay = by + j;
						found = true;
					}
				}
			}
			for (int32_t ring = 0; ring <= 2 && !found && !dense; ring++) {
				for (int32_t j = -ring; j <= ring && !found; j++) {
					for (int32_t i = -ring; i <= ring && !found; i++) {
						if (std::abs(i) != ring && std::abs(j) != ring) {
							continue;
						}
						if (fixed_point(bx + i, by + j)) {
							ax = bx + i;
							ay = by + j;
							found = true;
						}
					}
				}
			}
			if (!found) {
				continue; // this cell has no reachable phase-correct point
			}
			const std::pair<int32_t, int32_t> key{ax, ay};
			if (dense) {
				const int32_t ix = ax - (cx - half - dpad), iy = ay - (cy - half - dpad);
				if (ix >= 0 && iy >= 0 && ix < dspan && iy < dspan) {
					uint8_t &t = taken[(size_t)iy * dspan + ix];
					if (!t) {
						t = 1;
						r.dxs.push_back(ax);
						r.dys.push_back(ay);
					}
					continue;
				}
				// An answer further out than any snap reaches: fall through to
				// the local dedup below.
			}
			bool dup = false;
			for (auto it = seen.rbegin(); it != seen.rend() && it - seen.rbegin() < 8; ++it) {
				if (*it == key) {
					dup = true;
					break;
				}
			}
			if (!dup) {
				seen.push_back(key);
				r.dxs.push_back(key.first);
				r.dys.push_back(key.second);
			}
		}
	}
	return r;
}

/*
 *
 * Bulk path: the same probe, answered from a few grid snaps (runtime#1723).
 *
 */

/*!
 * One grid point's answer: the snapped displacement MINUS its target, device
 * px. Both fields @ref kGridNoAnswer = no answer for this point (a displacement
 * the transport cannot carry); the probe then asks that point singly.
 */
struct GridPoint
{
	int32_t dx = 0, dy = 0;
};
constexpr int32_t kGridNoAnswer = INT32_MIN;

/*!
 * A regular grid of DEVICE displacement targets (origin (0, 0)): point (i, j)
 * targets (first_x + i * step_x, first_y + j * step_y); answers are row-major,
 * point (i, j) at j * count_x + i.
 */
struct GridSpec
{
	int32_t first_x = 0, first_y = 0;
	int32_t step_x = 1, step_y = 1;
	uint32_t count_x = 0, count_y = 0;
};

/*!
 * A grid snap: fill count_x * count_y answers and @p declined (the display
 * processor produced no snap at all — the per-point snap's `false`). Returns
 * false when the grid cannot be evaluated (no entry point, a failed call);
 * the answers are then ignored.
 */
using GridSnapFn = std::function<bool(const GridSpec &, GridPoint *, bool *declined)>;
//! The per-point snap probe() takes (see there). May be empty.
using PointSnapFn = std::function<bool(int32_t tx, int32_t ty, int32_t *ox, int32_t *oy)>;

//! Largest count along one axis of one grid call (= the runtime's
//! XR_WEAVE_SNAP_GRID_MAX_AXIS_DXR; 1024 x 1024 is also its point cap).
constexpr uint32_t kGridMaxAxis = 1024;
/*!
 * Logical px beyond the probe grid, either side, that the fetched grids cover
 * at a non-unit scale. There the probe's reachable-lattice search asks about
 * logical points up to 2 px around the DP's answer mapped back to logical, and
 * the answer is itself a few device px off its target (a correct snap stays
 * within ~2); 6 covers a 3 px snap at any scale >= 1 with room to spare. A
 * query outside is still answered — singly — so this sets speed, not results.
 */
constexpr int32_t kGridPad = 6;
/*!
 * Relative cost of one grid call, in grid points: an IPC round trip (~0.15 ms)
 * against the runtime's per-point evaluation (~0.1-0.2 us). Only used to choose
 * between two covers of the same targets.
 */
constexpr uint64_t kGridCallCost = 1024;

//! An arithmetic progression of device displacements along one axis.
struct AxisRun
{
	int32_t first = 0, step = 1;
	uint32_t count = 0;
};

/*!
 * The DEVICE displacements, along one axis, of every logical displacement the
 * probe centred on @p c can query from start @p rel0: sorted, unique, split in
 * two segments — targets whose monitor-relative position is negative, and the
 * rest. (Mutter rounds halves AWAY from zero, so the rounding pattern mirrors
 * at the monitor edge and a period found on one side does not hold across it.
 * A window straddles the edge only while partly off the monitor.)
 *
 * At scale 1 exactly the targets are the probe grid alone (step @p cell): the
 * DP's answer is always reachable exactly (logical == device), so the search
 * around it never runs. Anywhere else they are every logical px of the grid's
 * span plus @ref kGridPad either side — the search queries logical neighbours
 * of the answer, which are off the grid.
 */
inline std::vector<std::vector<int32_t>>
axis_targets(int32_t rel0, double scale, int32_t c, int32_t half, int32_t cell)
{
	std::vector<int32_t> seg[2];
	const int32_t base = logical_to_px(rel0, scale);
	const int32_t step = scale == 1.0 ? cell : 1;
	const int32_t pad = scale == 1.0 ? 0 : kGridPad;
	for (int32_t l = c - half - pad; l <= c + half + pad; l += step) {
		seg[rel0 + l >= 0 ? 1 : 0].push_back(logical_to_px(rel0 + l, scale) - base);
	}
	std::vector<std::vector<int32_t>> out;
	for (auto &v : seg) {
		if (v.empty()) {
			continue;
		}
		std::sort(v.begin(), v.end());
		v.erase(std::unique(v.begin(), v.end()), v.end());
		out.push_back(std::move(v));
	}
	return out;
}

//! Append runs of step @p step covering first + k * step, k < count, each <= kGridMaxAxis long.
inline void
append_runs(std::vector<AxisRun> *runs, int32_t first, int32_t step, uint32_t count)
{
	for (uint32_t done = 0; done < count; done += kGridMaxAxis) {
		AxisRun r;
		r.first = first + (int32_t)done * step;
		r.step = step;
		r.count = std::min(kGridMaxAxis, count - done);
		runs->push_back(r);
	}
}

/*!
 * Exact cover of one segment (sorted, unique) by index residue. The mapping is
 * `round(x * scale)`, so at a rational scale p/q a logical span of q is a
 * device span of exactly p and the targets repeat their gaps with period q:
 * 1.5 -> gaps 2,1,2,1 (two runs of step 3), 1.25 -> 1,1,1,2 (four of step 5),
 * 5/3 -> 2,1,2 (three of step 5); one run at an integer scale. Every run point
 * is a target. A period is accepted only when it holds over the WHOLE segment
 * (float rounding at a non-dyadic scale can break one); none within 8 =
 * false.
 */
inline bool
cover_segment_periodic(const std::vector<int32_t> &v, std::vector<AxisRun> *runs)
{
	const size_t n = v.size();
	if (n == 1) {
		append_runs(runs, v[0], 1, 1);
		return true;
	}
	for (size_t q = 1; q <= 8 && q < n; q++) {
		bool ok = true;
		for (size_t i = 0; i + q + 1 < n && ok; i++) {
			ok = (v[i + 1] - v[i]) == (v[i + q + 1] - v[i + q]);
		}
		if (!ok) {
			continue;
		}
		const int32_t step = v[q] - v[0];
		for (size_t r = 0; r < q; r++) {
			append_runs(runs, v[r], step, (uint32_t)((n - r + q - 1) / q));
		}
		return true;
	}
	return false;
}

/*!
 * Cover one axis's targets (axis_targets()) with arithmetic progressions.
 * @p periodic: the exact per-segment residue cover (empty when a segment has
 * no period). Otherwise every device px from the first target to the last —
 * a superset, so it covers any target list.
 */
inline std::vector<AxisRun>
cover_axis(const std::vector<std::vector<int32_t>> &segs, bool periodic)
{
	std::vector<AxisRun> runs;
	if (segs.empty()) {
		return runs;
	}
	if (periodic) {
		// Across the edge first: it holds whenever nothing rounds a half
		// (every integer scale), and is then half as many runs.
		if (segs.size() > 1) {
			std::vector<int32_t> all;
			for (const auto &v : segs) {
				all.insert(all.end(), v.begin(), v.end());
			}
			std::sort(all.begin(), all.end());
			all.erase(std::unique(all.begin(), all.end()), all.end());
			if (cover_segment_periodic(all, &runs)) {
				return runs;
			}
			runs.clear();
		}
		for (const auto &v : segs) {
			if (!cover_segment_periodic(v, &runs)) {
				return {};
			}
		}
		return runs;
	}
	int32_t lo = INT32_MAX, hi = INT32_MIN;
	for (const auto &v : segs) {
		lo = std::min(lo, v.front());
		hi = std::max(hi, v.back());
	}
	append_runs(&runs, lo, 1, (uint32_t)(hi - lo + 1));
	return runs;
}

//! Points a cover evaluates.
inline uint64_t
cover_points(const std::vector<AxisRun> &runs)
{
	uint64_t n = 0;
	for (const auto &r : runs) {
		n += r.count;
	}
	return n;
}

/*!
 * The grid calls that answer every query probe() makes from @p m around
 * (@p cx, @p cy): the product of one axis cover per axis. The targets are
 * SEPARABLE — a logical (lx, ly) goes to (fx(lx), fy(ly)) — so a product of
 * two 1-D covers covers them.
 *
 * The targets are NOT a regular grid at a fractional scale (1.5: device gaps
 * 2,1,2,1…), so one call cannot name exactly them. Per axis, two covers are
 * weighed, counting @ref kGridCallCost points per call:
 *  - exact: one run per residue class of the rounding's period (and per side
 *    of the monitor edge when the targets straddle it) — cover_axis();
 *  - dense: every device px of the span, one call (a superset).
 *
 * Measured by tests/linux_window_lattice_test.cpp on the helper's real shape
 * (129 x 129, ±192 logical, 157,609 points to answer at a non-unit scale):
 * 100 % and 200 % -> ONE call; 150 % -> 4; 166.67 % -> 9; 125 % and 175 % ->
 * 16 (more, at most 64, while the window straddles the monitor's edge). The
 * dense table (cell 1, the helper's grid path since runtime#1748) asks for the
 * same calls and points at a non-unit scale — those already cover every
 * logical px — and at 100 % for one call of 385 x 385 (148,225 points).
 */
inline std::vector<GridSpec>
plan_grids(const Map &m, int32_t cx, int32_t cy, int32_t half, int32_t cell)
{
	const auto tx = axis_targets(m.rel0_x, m.scale, cx, half, cell);
	const auto ty = axis_targets(m.rel0_y, m.scale, cy, half, cell);
	const std::vector<AxisRun> xs[2] = {cover_axis(tx, true), cover_axis(tx, false)};
	const std::vector<AxisRun> ys[2] = {cover_axis(ty, true), cover_axis(ty, false)};
	int best_x = -1, best_y = -1;
	uint64_t best = UINT64_MAX;
	for (int a = 0; a < 2; a++) {
		for (int b = 0; b < 2; b++) {
			if (xs[a].empty() || ys[b].empty()) {
				continue;
			}
			const uint64_t calls = (uint64_t)xs[a].size() * ys[b].size();
			const uint64_t cost = cover_points(xs[a]) * cover_points(ys[b]) + kGridCallCost * calls;
			if (cost < best) {
				best = cost;
				best_x = a;
				best_y = b;
			}
		}
	}
	std::vector<GridSpec> out;
	if (best_x < 0) {
		return out;
	}
	for (const auto &ry : ys[best_y]) {
		for (const auto &rx : xs[best_x]) {
			GridSpec g;
			g.first_x = rx.first;
			g.first_y = ry.first;
			g.step_x = rx.step;
			g.step_y = ry.step;
			g.count_x = rx.count;
			g.count_y = ry.count;
			out.push_back(g);
		}
	}
	return out;
}

/*!
 * probe(), with the display processor asked through a few GRID calls instead
 * of one call per point: plan_grids() names them, their answers are memoised,
 * and probe() runs unchanged against the memo. The table is therefore the
 * per-point probe's table by construction — the same function over the same
 * answers — as long as the grid snap answers each point exactly as the
 * per-point snap would (the runtime's grid entry point is that loop, next to
 * the DP).
 *
 * A query the memo lacks (outside the planned cover, or a point answered with
 * @ref kGridNoAnswer) goes to @p point, or, without one, to a 1 x 1 grid.
 *
 * Falls back to the per-point probe (and says why in Probe::grid_fallback)
 * when a grid call fails, or reports the DP declined — the per-point probe
 * then declines on its first query unless the DP has come up since, which is
 * the same table or a better one. Without @p point there is nothing to fall
 * back to: a failed grid is then an unanswered probe (declined).
 *
 * @p fallback_cell (0 = @p cell) is the cell that per-point probe uses. The
 * dense table (@p cell 1) is ~9x the queries of a 3 px one: free through a
 * grid call, which evaluates every logical px of the window at a non-unit
 * scale anyway, but seconds of round trips one point at a time. So a caller
 * asking for the dense table names a coarser cell for the per-point fallback.
 */
inline Probe
probe_via_grid(const GridSnapFn &grid,
               const PointSnapFn &point,
               const Map &m,
               int32_t cx,
               int32_t cy,
               int32_t half,
               int32_t cell,
               int32_t fallback_cell = 0)
{
	auto fallback = [&](const char *why, uint32_t calls, uint64_t points) {
		Probe r;
		if (point) {
			r = probe(point, m, cx, cy, half, fallback_cell > 0 ? fallback_cell : cell);
		} else {
			r.declined = true;
		}
		r.grid_calls = calls;
		r.grid_points = points;
		r.grid_fallback = why;
		return r;
	};

	const std::vector<GridSpec> plan = plan_grids(m, cx, cy, half, cell);
	if (plan.empty()) {
		return fallback("no grid covers the probe", 0, 0);
	}

	// Memo over the union of the covered values, per axis.
	std::vector<int32_t> mx, my;
	for (const auto &g : plan) {
		for (uint32_t i = 0; i < g.count_x; i++) {
			mx.push_back(g.first_x + (int32_t)i * g.step_x);
		}
		for (uint32_t j = 0; j < g.count_y; j++) {
			my.push_back(g.first_y + (int32_t)j * g.step_y);
		}
	}
	for (auto *v : {&mx, &my}) {
		std::sort(v->begin(), v->end());
		v->erase(std::unique(v->begin(), v->end()), v->end());
	}
	auto index_of = [](const std::vector<int32_t> &v, int32_t x) -> int64_t {
		const auto it = std::lower_bound(v.begin(), v.end(), x);
		return (it != v.end() && *it == x) ? (int64_t)(it - v.begin()) : -1;
	};
	std::vector<GridPoint> memo(mx.size() * my.size());
	std::vector<uint8_t> have(memo.size(), 0);

	uint32_t calls = 0;
	uint64_t points = 0;
	std::vector<GridPoint> buf;
	for (const auto &g : plan) {
		buf.assign((size_t)g.count_x * g.count_y, GridPoint{});
		bool declined = false;
		calls++;
		if (!grid(g, buf.data(), &declined)) {
			return fallback("the grid snap failed", calls, points);
		}
		if (declined) {
			return fallback("the grid snap reported the display processor declined", calls, points);
		}
		points += buf.size();
		for (uint32_t j = 0; j < g.count_y; j++) {
			const size_t row = (size_t)index_of(my, g.first_y + (int32_t)j * g.step_y) * mx.size();
			for (uint32_t i = 0; i < g.count_x; i++) {
				const GridPoint &p = buf[(size_t)j * g.count_x + i];
				if (p.dx == kGridNoAnswer || p.dy == kGridNoAnswer) {
					continue;
				}
				const size_t k = row + (size_t)index_of(mx, g.first_x + (int32_t)i * g.step_x);
				memo[k] = p;
				have[k] = 1;
			}
		}
	}

	uint32_t misses = 0;
	auto snap = [&](int32_t tx, int32_t ty, int32_t *ox, int32_t *oy) {
		const int64_t i = index_of(mx, tx), j = index_of(my, ty);
		if (i >= 0 && j >= 0) {
			const size_t k = (size_t)j * mx.size() + (size_t)i;
			if (have[k]) {
				*ox = tx + memo[k].dx;
				*oy = ty + memo[k].dy;
				return true;
			}
		}
		misses++;
		if (point) {
			return point(tx, ty, ox, oy);
		}
		GridSpec one;
		one.first_x = tx;
		one.first_y = ty;
		one.count_x = one.count_y = 1;
		GridPoint p;
		bool declined = false;
		calls++;
		if (!grid(one, &p, &declined) || declined || p.dx == kGridNoAnswer || p.dy == kGridNoAnswer) {
			return false;
		}
		points++;
		*ox = tx + p.dx;
		*oy = ty + p.dy;
		return true;
	};
	Probe r = probe(snap, m, cx, cy, half, cell);
	r.grid_used = true;
	r.grid_calls = calls;
	r.grid_points = points;
	r.grid_misses = misses;
	return r;
}

} // namespace dxr_wl_lattice

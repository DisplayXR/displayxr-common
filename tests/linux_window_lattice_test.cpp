// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The Wayland drag lattice's table build at any output scale
 *         (common/linux/dxr_wl_lattice.h). No display, no compositor.
 *
 * 1. The logical -> device mapping is Mutter's (round half away from zero on
 *    the monitor-relative position) at 100 %, 125 %, 150 %, 166.67 %, 175 %
 *    and 200 %, including negative positions.
 * 2. At an integer scale the table is IDENTICAL to the integer-only build it
 *    replaced (reimplemented below from the pre-follow-up helper).
 * 3. At fractional scales every table entry is reachable and phase-correct
 *    under a synthetic slanted-lens snap, and entries cover the grid.
 * 4. The bulk path (probe_via_grid, runtime#1723) builds the SAME table as the
 *    per-point probe, at 100 %, 150 % and 200 % (and 125 %, 166.67 %, 175 %),
 *    in the helper's real shape: ONE grid call at an integer scale, one per
 *    residue class at a fractional one, no point asked singly. Its fallbacks —
 *    a failing grid, a declining one, unanswerable points, no per-point
 *    provider — give the same table too.
 * 5. The dense table (cell 1, runtime#1748) under a best-phase lens snap: no
 *    duplicates, a superset of the 3 px table inside the window, every
 *    reachable fixed point present, a smaller largest pull at 150 % and 200 %,
 *    the same table through the grid snap, and a failing grid falling back to
 *    single points at the coarse cell.
 */
#include "dxr_wl_lattice.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <utility>

namespace L = dxr_wl_lattice;

static int g_fail = 0;
#define CHECK(c)                                                                                                       \
	do {                                                                                                           \
		if (!(c)) {                                                                                            \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                              \
			g_fail++;                                                                                      \
		}                                                                                                      \
	} while (0)

//! A slanted-lens stand-in for a display processor's snap: a device position
//! is phase-correct when (x + 2y) is a multiple of 5 (displacement from the
//! drag start). The snap returns the nearest such x on the same row.
static bool
synthetic_snap(int32_t tx, int32_t ty, int32_t *ox, int32_t *oy)
{
	*oy = ty;
	int32_t best = tx;
	for (int32_t d = 0; d <= 5; d++) {
		if ((((tx + d) + 2 * ty) % 5 + 5) % 5 == 0) {
			best = tx + d;
			break;
		}
		if ((((tx - d) + 2 * ty) % 5 + 5) % 5 == 0) {
			best = tx - d;
			break;
		}
	}
	*ox = best;
	return true;
}

//! The integer-only build the helper used before (device quantum q).
static L::Probe
old_integer_build(int32_t q, int32_t cx, int32_t cy, int32_t half, int32_t cell)
{
	auto round_to_multiple = [](int32_t v, int32_t qq) {
		const int32_t h = qq / 2;
		return v >= 0 ? ((v + h) / qq) * qq : -(((-v + h) / qq) * qq);
	};
	L::Probe r;
	std::vector<std::pair<int32_t, int32_t>> seen;
	for (int32_t gy = cy - half; gy <= cy + half; gy += cell) {
		for (int32_t gx = cx - half; gx <= cx + half; gx += cell) {
			r.probed++;
			int32_t sx = gx * q, sy = gy * q;
			synthetic_snap(gx * q, gy * q, &sx, &sy);
			if (sx == gx * q && sy == gy * q) {
				r.fixed++;
			}
			int32_t ax = 0, ay = 0;
			bool found = false;
			if (sx % q == 0 && sy % q == 0) {
				ax = sx;
				ay = sy;
				found = true;
			} else {
				const int32_t bx = round_to_multiple(sx, q), by = round_to_multiple(sy, q);
				for (int32_t ring = 0; ring <= 2 && !found; ring++) {
					for (int32_t j = -ring; j <= ring && !found; j++) {
						for (int32_t i = -ring; i <= ring && !found; i++) {
							if (std::abs(i) != ring && std::abs(j) != ring) {
								continue;
							}
							const int32_t px = bx + i * q, py = by + j * q;
							int32_t rx = px, ry = py;
							if (synthetic_snap(px, py, &rx, &ry) && rx == px && ry == py) {
								ax = px;
								ay = py;
								found = true;
							}
						}
					}
				}
			}
			if (!found) {
				continue;
			}
			const std::pair<int32_t, int32_t> key{ax / q, ay / q};
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

int
main()
{
	// 1. Mutter's mapping.
	CHECK(L::logical_to_px(0, 1.0) == 0);
	CHECK(L::logical_to_px(7, 1.0) == 7);
	CHECK(L::logical_to_px(1, 1.25) == 1);  // 1.25
	CHECK(L::logical_to_px(2, 1.25) == 3);  // 2.5 -> away from zero
	CHECK(L::logical_to_px(-2, 1.25) == -3);
	CHECK(L::logical_to_px(1, 1.5) == 2);   // 1.5 -> 2
	CHECK(L::logical_to_px(2, 1.5) == 3);
	CHECK(L::logical_to_px(3, 1.5) == 5);   // 4.5 -> 5
	CHECK(L::logical_to_px(-1, 1.5) == -2);
	CHECK(L::logical_to_px(1, 5.0 / 3.0) == 2); // 1.667
	CHECK(L::logical_to_px(2, 5.0 / 3.0) == 3); // 3.333
	CHECK(L::logical_to_px(3, 5.0 / 3.0) == 5);
	CHECK(L::logical_to_px(2, 1.75) == 4);  // 3.5 -> 4
	CHECK(L::logical_to_px(3, 1.75) == 5);  // 5.25
	CHECK(L::logical_to_px(123, 2.0) == 246);
	CHECK(L::logical_to_px(-123, 2.0) == -246);

	// The device displacement of a logical move depends on the start at a
	// fractional scale (the rounding), and not at an integer one.
	{
		int32_t a = 0, b = 0, c = 0, d = 0;
		L::Map even{0, 0, 1.5}, odd{1, 0, 1.5};
		L::device_displacement(even, 1, 0, &a, &b); // 0 -> 1 logical: 0 -> 2 px
		L::device_displacement(odd, 1, 0, &c, &d);  // 1 -> 2 logical: 2 -> 3 px
		CHECK(a == 2 && c == 1);
		L::Map i0{0, 0, 2.0}, i1{1, 0, 2.0};
		L::device_displacement(i0, 5, 3, &a, &b);
		L::device_displacement(i1, 5, 3, &c, &d);
		CHECK(a == 10 && b == 6 && c == 10 && d == 6);
	}

	// 2. At 200 % (and 100 %, 300 %) the table is identical to the old build,
	//    whatever the start.
	auto snap = [](int32_t tx, int32_t ty, int32_t *ox, int32_t *oy) { return synthetic_snap(tx, ty, ox, oy); };
	for (int32_t q = 1; q <= 3; q++) {
		for (int32_t rel0 = -3; rel0 <= 3; rel0++) {
			const L::Map m{rel0, 2 * rel0 + 1, (double)q};
			for (auto c : {std::pair<int32_t, int32_t>{0, 0}, {40, -17}}) {
				const L::Probe a = L::probe(snap, m, c.first, c.second, 24, 3);
				const L::Probe b = old_integer_build(q, c.first, c.second, 24, 3);
				CHECK(a.dxs == b.dxs);
				CHECK(a.dys == b.dys);
				CHECK(a.probed == b.probed && a.fixed == b.fixed);
			}
		}
	}

	// 3. Fractional scales: every entry is reachable (it IS a logical
	//    displacement) and phase-correct at the device position it produces,
	//    and most cells have one.
	for (double s : {1.25, 1.5, 5.0 / 3.0, 1.75}) {
		for (int32_t rel0 = 0; rel0 < 4; rel0++) {
			const L::Map m{rel0 + 100, rel0 + 37, s};
			const L::Probe p = L::probe(snap, m, 0, 0, 30, 3);
			CHECK(!p.dxs.empty());
			CHECK(p.dxs.size() * 2 >= p.probed); // at least half the cells
			for (size_t i = 0; i < p.dxs.size(); i++) {
				int32_t tx = 0, ty = 0;
				L::device_displacement(m, p.dxs[i], p.dys[i], &tx, &ty);
				int32_t ox = 0, oy = 0;
				synthetic_snap(tx, ty, &ox, &oy);
				CHECK(ox == tx && oy == ty);
			}
		}
	}

	// 4. The bulk path.
	{
		// The helper's real probe shape (dxr_linux_window.cpp kLatticeHalf /
		// kLatticeCell).
		const int32_t half = 192, cell = 3;

		struct Counts
		{
			uint64_t point = 0, grid = 0, grid_points = 0;
		};
		// A grid snap that is literally the per-point snap in a loop — what
		// the runtime's xrWeaveSnapWindowGridDXR is (u_snap_grid_eval).
		auto make_grid = [](Counts *c, const L::PointSnapFn &one) {
			return [c, one](const L::GridSpec &g, L::GridPoint *out, bool *declined) {
				c->grid++;
				c->grid_points += (uint64_t)g.count_x * g.count_y;
				CHECK(g.step_x >= 1 && g.step_y >= 1);
				CHECK(g.count_x >= 1 && g.count_x <= L::kGridMaxAxis);
				CHECK(g.count_y >= 1 && g.count_y <= L::kGridMaxAxis);
				*declined = false;
				for (uint32_t j = 0; j < g.count_y; j++) {
					for (uint32_t i = 0; i < g.count_x; i++) {
						const int32_t tx = g.first_x + (int32_t)i * g.step_x;
						const int32_t ty = g.first_y + (int32_t)j * g.step_y;
						int32_t ox = tx, oy = ty;
						if (!one(tx, ty, &ox, &oy)) {
							*declined = true;
							return true;
						}
						out[(size_t)j * g.count_x + i] = L::GridPoint{ox - tx, oy - ty};
					}
				}
				return true;
			};
		};
		auto same = [](const L::Probe &a, const L::Probe &b) {
			return a.dxs == b.dxs && a.dys == b.dys && a.probed == b.probed && a.fixed == b.fixed &&
			       a.declined == b.declined;
		};

		struct Case
		{
			double scale;
			uint32_t calls; // expected grid calls with every target on the monitor
		};
		for (const Case sc : {Case{1.0, 1}, Case{1.5, 4}, Case{2.0, 1}, Case{1.25, 16}, Case{5.0 / 3.0, 9},
		                      Case{1.75, 16}}) {
			for (int32_t rel0 = 0; rel0 < 3; rel0++) {
				// The first map keeps every target on the monitor (the call
				// counts below); the second straddles its top-left edge,
				// where the rounding mirrors.
				const L::Map maps[2] = {{rel0 + 311, 2 * rel0 + 257, sc.scale},
				                        {rel0 - 7, 2 * rel0 + 3, sc.scale}};
				for (int mi = 0; mi < 2; mi++) {
					const L::Map m = maps[mi];
					for (auto c : {std::pair<int32_t, int32_t>{0, 0}, {250, -40}}) {
						Counts n;
						L::PointSnapFn point = [&n](int32_t tx, int32_t ty, int32_t *ox, int32_t *oy) {
							n.point++;
							return synthetic_snap(tx, ty, ox, oy);
						};
						const L::Probe a = L::probe(snap, m, c.first, c.second, half, cell);
						const L::Probe b =
						    L::probe_via_grid(make_grid(&n, snap), point, m, c.first, c.second, half, cell);
						CHECK(same(a, b));
						CHECK(b.grid_used && b.grid_fallback == nullptr);
						CHECK(b.grid_misses == 0 && n.point == 0);
						CHECK(b.grid_calls == n.grid && b.grid_points == n.grid_points);
						if (mi == 0 || sc.scale == 1.0 || sc.scale == 2.0) {
							CHECK(n.grid == sc.calls);
						}
						CHECK(n.grid <= 64); // straddling the monitor edge at 175 %: 8 x 8 residue runs
						if (sc.scale == 1.0) {
							// Exactly the 129 x 129 probe grid.
							CHECK(n.grid_points == 129u * 129u);
						}
						std::printf("  bulk probe: scale %.4f rel0 (%d, %d) centre (%+d, %+d): %zu entries, "
						            "%u grid call(s), %llu points, %u single — identical to per point\n",
						            sc.scale, m.rel0_x, m.rel0_y, c.first, c.second, b.dxs.size(), b.grid_calls,
						            (unsigned long long)b.grid_points, b.grid_misses);
					}
				}
			}
		}

		// A snap reaching further than the planned cover (±8 px): the
		// queries outside it are asked singly and the table is unchanged.
		auto far_snap = [](int32_t tx, int32_t ty, int32_t *ox, int32_t *oy) {
			*oy = ty;
			const int32_t r = ((tx + 3 * ty) % 17 + 17) % 17; // phase-correct when 0
			*ox = r <= 8 ? tx - r : tx + (17 - r);
			return true;
		};
		for (double s : {1.0, 1.5, 2.0}) {
			const L::Map m{7, 3, s};
			Counts n;
			const L::PointSnapFn point = far_snap;
			const L::Probe a = L::probe(far_snap, m, 0, 0, half, cell);
			const L::Probe b = L::probe_via_grid(make_grid(&n, far_snap), point, m, 0, 0, half, cell);
			CHECK(same(a, b));
			CHECK(b.grid_used);
		}

		for (double s : {1.0, 1.5, 2.0}) {
			const L::Map m{5, 9, s};
			const L::Probe a = L::probe(snap, m, 0, 0, half, cell);

			// Grid call fails -> the per-point probe.
			{
				auto grid = [](const L::GridSpec &, L::GridPoint *, bool *) { return false; };
				const L::Probe b = L::probe_via_grid(grid, snap, m, 0, 0, half, cell);
				CHECK(same(a, b) && !b.grid_used && b.grid_fallback != nullptr);
			}
			// Grid reports the DP declined, but the DP answers by the time
			// the per-point probe runs -> the per-point table.
			{
				auto grid = [](const L::GridSpec &, L::GridPoint *, bool *d) {
					*d = true;
					return true;
				};
				const L::Probe b = L::probe_via_grid(grid, snap, m, 0, 0, half, cell);
				CHECK(same(a, b) && !b.grid_used && b.grid_fallback != nullptr);
			}
			// DP declines on both paths -> declined on both.
			{
				auto no = [](int32_t, int32_t, int32_t *, int32_t *) { return false; };
				Counts n;
				const L::Probe d1 = L::probe(no, m, 0, 0, half, cell);
				const L::Probe d2 = L::probe_via_grid(make_grid(&n, no), no, m, 0, 0, half, cell);
				CHECK(d1.declined && d2.declined && same(d1, d2));
			}
			// Some points unanswerable (the runtime's NO_DELTA) -> asked singly.
			{
				Counts n;
				auto inner = make_grid(&n, snap);
				auto grid = [inner](const L::GridSpec &g, L::GridPoint *out, bool *d) {
					if (!inner(g, out, d)) {
						return false;
					}
					for (size_t k = 0; k < (size_t)g.count_x * g.count_y; k += 7) {
						out[k] = L::GridPoint{L::kGridNoAnswer, L::kGridNoAnswer};
					}
					return true;
				};
				const L::Probe b = L::probe_via_grid(grid, snap, m, 0, 0, half, cell);
				CHECK(same(a, b) && b.grid_used && b.grid_misses > 0);
			}
			// Grid provider only (no per-point one): misses become 1 x 1 grids.
			{
				Counts n;
				auto inner = make_grid(&n, snap);
				auto grid = [inner](const L::GridSpec &g, L::GridPoint *out, bool *d) {
					if (!inner(g, out, d)) {
						return false;
					}
					if (g.count_x * g.count_y > 1) {
						out[0] = L::GridPoint{L::kGridNoAnswer, L::kGridNoAnswer};
					}
					return true;
				};
				const L::Probe b = L::probe_via_grid(grid, L::PointSnapFn{}, m, 0, 0, half, cell);
				CHECK(same(a, b) && b.grid_used);
			}
		}

		// Every plan stays inside the runtime's per-call bounds, up to 300 %.
		for (double s = 1.0; s <= 3.0; s += 0.05) {
			const L::Map m{123, 45, s};
			const auto plan = L::plan_grids(m, 0, 0, half, cell);
			CHECK(!plan.empty());
			for (const auto &g : plan) {
				CHECK(g.step_x >= 1 && g.step_y >= 1);
				CHECK(g.count_x >= 1 && g.count_x <= L::kGridMaxAxis);
				CHECK(g.count_y >= 1 && g.count_y <= L::kGridMaxAxis);
			}
		}

	// 5. The dense table (cell 1, runtime#1748), against a best-phase snap
	//    shaped like a vendor weaver's: the position within ±2 device px of
	//    the target whose phase (x + slant * y) / period best matches the
	//    start's. The lens numbers are a test double's, nothing more.
	{
		auto phase_err = [](int32_t x, int32_t y) {
			return std::fabs(std::remainder((x + 0.287 * y) / 2.76, 1.0));
		};
		auto lens_snap = [phase_err](int32_t tx, int32_t ty, int32_t *ox, int32_t *oy) {
			double best = 1e9;
			for (int32_t i = tx - 2; i <= tx + 2; i++) {
				for (int32_t j = ty - 2; j <= ty + 2; j++) {
					const double e = phase_err(i, j);
					if (e < best) {
						best = e;
						*ox = i;
						*oy = j;
					}
				}
			}
			return true;
		};
		const L::PointSnapFn lens_point = lens_snap;
		const int32_t half = 60;
		for (double s : {1.0, 1.25, 1.5, 2.0}) {
			const L::Map m{311, 257, s};
			const L::Probe coarse = L::probe(lens_snap, m, 0, 0, half, 3);
			const L::Probe dense = L::probe(lens_snap, m, 0, 0, half, 1);
			std::set<std::pair<int32_t, int32_t>> d, c;
			for (size_t i = 0; i < dense.dxs.size(); i++) {
				CHECK(d.insert({dense.dxs[i], dense.dys[i]}).second); // no duplicate
			}
			for (size_t i = 0; i < coarse.dxs.size(); i++) {
				c.insert({coarse.dxs[i], coarse.dys[i]});
			}
			CHECK(dense.probed == (size_t)(2 * half + 1) * (2 * half + 1));
			// Everything the coarse table has inside the window, the dense one has.
			for (const auto &e : c) {
				if (std::abs(e.first) <= half && std::abs(e.second) <= half) {
					CHECK(d.count(e) == 1);
				}
			}
			// Every reachable position the snap leaves in place is an entry,
			// and every entry is as phase-correct as the snap's own answers.
			for (int32_t y = -half; y <= half; y++) {
				for (int32_t x = -half; x <= half; x++) {
					int32_t tx = 0, ty = 0, ox = 0, oy = 0;
					L::device_displacement(m, x, y, &tx, &ty);
					lens_snap(tx, ty, &ox, &oy);
					if (ox == tx && oy == ty) {
						CHECK(d.count({x, y}) == 1);
					}
				}
			}
			for (const auto &e : d) {
				int32_t tx = 0, ty = 0;
				L::device_displacement(m, e.first, e.second, &tx, &ty);
				CHECK(phase_err(tx, ty) < 0.05);
			}
			// The window's largest pull, in DEVICE px, over positions well
			// inside the table: never worse, and at a non-unit scale better.
			auto worst_pull = [&m](const std::set<std::pair<int32_t, int32_t>> &t) {
				double worst = 0;
				for (int32_t y = -half + 8; y <= half - 8; y++) {
					for (int32_t x = -half + 8; x <= half - 8; x++) {
						int32_t rx = 0, ry = 0;
						L::device_displacement(m, x, y, &rx, &ry);
						double best = 1e18;
						for (const auto &e : t) {
							if (std::abs(e.first - x) > 6 || std::abs(e.second - y) > 6) {
								continue;
							}
							int32_t ex = 0, ey = 0;
							L::device_displacement(m, e.first, e.second, &ex, &ey);
							best = std::min(best, std::hypot(ex - rx, ey - ry));
						}
						worst = std::max(worst, best);
					}
				}
				return worst;
			};
			const double pull_c = worst_pull(c), pull_d = worst_pull(d);
			CHECK(pull_d <= pull_c);
			if (s == 1.5 || s == 2.0) {
				CHECK(pull_d < pull_c);
			}
			std::printf("  dense table: scale %.2f: %zu entries (coarse %zu), largest pull %.2f device px (coarse "
			            "%.2f)\n",
			            s, d.size(), c.size(), pull_d, pull_c);

			// Through the grid snap: the same dense table, in the same number
			// of calls as the coarse one at a non-unit scale.
			Counts n;
			const L::Probe g = L::probe_via_grid(make_grid(&n, lens_point), lens_point, m, 0, 0, half, 1, 3);
			CHECK(same(g, dense) && g.grid_used && g.grid_misses == 0);
			if (s == 1.0 || s == 2.0) {
				CHECK(n.grid == 1);
			}
			// A grid that fails falls back to single points at the COARSE cell.
			auto broken = [](const L::GridSpec &, L::GridPoint *, bool *) { return false; };
			const L::Probe f = L::probe_via_grid(broken, lens_point, m, 0, 0, half, 1, 3);
			CHECK(same(f, coarse) && !f.grid_used && f.grid_fallback != nullptr);
		}
	}
	} // 4 + 5 share the grid test doubles

	if (g_fail == 0) {
		std::printf("linux_window_lattice_test: all checks passed\n");
	}
	return g_fail == 0 ? 0 : 1;
}

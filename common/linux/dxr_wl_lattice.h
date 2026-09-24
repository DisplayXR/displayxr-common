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

#include <cmath>
#include <cstdint>
#include <cstdlib>
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
 */
template <typename Snap>
inline Probe
probe(const Snap &snap, const Map &m, int32_t cx, int32_t cy, int32_t half, int32_t cell)
{
	Probe r;
	std::vector<std::pair<int32_t, int32_t>> seen;
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
			for (int32_t ring = 0; ring <= 2 && !found; ring++) {
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

} // namespace dxr_wl_lattice

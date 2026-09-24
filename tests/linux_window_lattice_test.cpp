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
 */
#include "dxr_wl_lattice.h"

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

	if (g_fail == 0) {
		std::printf("linux_window_lattice_test: all checks passed\n");
	}
	return g_fail == 0 ? 0 : 1;
}

// Copyright 2025-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Tiny CI consumer for displayxr::math.
 *
 * Links displayxr::math and runs:
 *   1. display3d_selftest() — the projection/unproject round-trip + NDC-
 *      orientation drift guard built into the library.
 *   2. a focused check of display3d_resolve_window_rect() — the window/canvas
 *      input-prep (Layer 1), whose screen-Y-down -> eye-Y-up flip is exactly the
 *      kind of thing that drifts when re-implemented per consumer.
 * A non-zero return fails CI.
 */

#include "display3d_view.h"

#include <math.h>
#include <stdio.h>

static int
near_eq(float a, float b)
{
	return fabsf(a - b) < 1e-5f;
}

static int
check_resolve_window_rect(void)
{
	// 1m x 1m display at 1000x1000 px => 0.001 m/px.
	// Rect: center (750, 250) px (right of, and ABOVE, display center in Y-down
	// pixels), size 500x500 px.
	Display3DWindowPlacement p;
	p.display_width_m = 1.0f;  p.display_height_m = 1.0f;
	p.display_width_px = 1000.0f; p.display_height_px = 1000.0f;
	p.rect_center_x_px = 750.0f; p.rect_center_y_px = 250.0f;
	p.rect_width_px = 500.0f; p.rect_height_px = 500.0f;

	XrVector3f raw[2] = {{0.0f, 0.0f, 0.6f}, {0.05f, 0.0f, 0.6f}};
	Display3DScreen screen;
	XrVector3f out[2];
	display3d_resolve_window_rect(&p, raw, 2, &screen, out);

	int fail = 0;
	// screen = rect size in meters.
	if (!near_eq(screen.width_m, 0.5f) || !near_eq(screen.height_m, 0.5f)) {
		fprintf(stderr, "resolve_window_rect: screen %.4f x %.4f, expected 0.5 x 0.5\n",
		        screen.width_m, screen.height_m);
		fail++;
	}
	// off_x = (750-500)*0.001 = +0.25 -> eye.x - off_x.
	// off_y = -((250-500)*0.001) = +0.25 (Y-flip) -> eye.y - off_y.
	if (!near_eq(out[0].x, -0.25f) || !near_eq(out[0].y, -0.25f) || !near_eq(out[0].z, 0.6f)) {
		fprintf(stderr, "resolve_window_rect: eye0 (%.4f,%.4f,%.4f), expected (-0.25,-0.25,0.6)\n",
		        out[0].x, out[0].y, out[0].z);
		fail++;
	}
	if (!near_eq(out[1].x, -0.20f)) { // 0.05 - 0.25
		fprintf(stderr, "resolve_window_rect: eye1.x %.4f, expected -0.20\n", out[1].x);
		fail++;
	}

	// A centered, full-display rect must leave eyes unchanged.
	Display3DWindowPlacement c = p;
	c.rect_center_x_px = 500.0f; c.rect_center_y_px = 500.0f;
	c.rect_width_px = 1000.0f; c.rect_height_px = 1000.0f;
	display3d_resolve_window_rect(&c, raw, 1, &screen, out);
	if (!near_eq(out[0].x, 0.0f) || !near_eq(out[0].y, 0.0f) ||
	    !near_eq(screen.width_m, 1.0f) || !near_eq(screen.height_m, 1.0f)) {
		fprintf(stderr, "resolve_window_rect: centered full-display rect should be identity\n");
		fail++;
	}
	return fail;
}

int
main(void)
{
	int failures = display3d_selftest();
	failures += check_resolve_window_rect();

	if (failures != 0) {
		fprintf(stderr, "displayxr::math selftest FAILED: %d check(s)\n", failures);
		return 1;
	}
	printf("displayxr::math selftest OK\n");
	return 0;
}

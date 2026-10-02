// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The two Wayland scale factors in both of mutter's layout modes
 *         (common/linux/dxr_wl_scale.h). No display, no compositor.
 *
 * Inputs are measured on a headless mutter 50.1 with one 3840x2160 virtual
 * monitor, switching only `gdctl set --layout-mode`:
 *
 *   layout     scale   wl_output.mode   xdg_output.logical_size   wl_output.scale
 *   physical   2       3840x2160        3840x2160                 2
 *   logical    2       3840x2160        1920x1080                 2
 *   logical    1.5     3840x2160        2560x1440                 2
 *
 * 1. The surface-scale estimate is the integer scale in PHYSICAL and the
 *    fractional stage factor in LOGICAL, so a 720x400 initial rect is a
 *    360x200 surface in both (it was 720x400 surface units — 1440x800 px —
 *    in PHYSICAL).
 * 2. The stage factor of a published monitor: the extension's device_scale /
 *    layout_mode first, then the client's own output, then the monitor scale.
 */
#include "dxr_wl_scale.h"

#include <cstdio>
#include <initializer_list>

static int g_fail = 0;
#define CHECK(c)                                                                                                       \
	do {                                                                                                           \
		if (!(c)) {                                                                                            \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                       \
			g_fail++;                                                                                      \
		}                                                                                                      \
	} while (0)

int
main()
{
	// 1. Surface-scale estimate.
	CHECK(dxr_wl_surface_scale_estimate(3840, 3840, 2) == 2.0); // physical at 2
	CHECK(dxr_wl_surface_scale_estimate(3840, 1920, 2) == 2.0); // logical at 2
	CHECK(dxr_wl_surface_scale_estimate(3840, 2560, 2) == 1.5); // logical at 1.5 (int scale says 2)
	CHECK(dxr_wl_surface_scale_estimate(3840, 3840, 1) == 1.0); // 100 %, either mode
	CHECK(dxr_wl_surface_scale_estimate(2880, 1728, 2) > 1.66 && dxr_wl_surface_scale_estimate(2880, 1728, 2) < 1.67);
	CHECK(dxr_wl_surface_scale_estimate(0, 1920, 2) == 1.0);    // nothing known
	CHECK(dxr_wl_surface_scale_estimate(3840, 0, 2) == 1.0);
	// A 720x400 device-px rect is a 360x200 surface in both modes.
	for (const int32_t logical_w : {3840, 1920}) {
		const double s = dxr_wl_surface_scale_estimate(3840, logical_w, 2);
		CHECK((int32_t)(720.0 / s + 0.5) == 360);
		CHECK((int32_t)(400.0 / s + 0.5) == 200);
	}

	// 2. Stage factor of a published monitor.
	// Extension v11 answers: authoritative.
	CHECK(dxr_wl_stage_factor(2.0, 1.0, U_WL_LAYOUT_MODE_PHYSICAL, 0.0) == 1.0);
	CHECK(dxr_wl_stage_factor(2.0, 2.0, U_WL_LAYOUT_MODE_LOGICAL, 0.0) == 2.0);
	CHECK(dxr_wl_stage_factor(2.0, 0.0, U_WL_LAYOUT_MODE_PHYSICAL, 2.0) == 1.0); // beats the output
	CHECK(dxr_wl_stage_factor(1.5, 1.5, U_WL_LAYOUT_MODE_UNKNOWN, 0.0) == 1.5);
	// An older extension: the client's own output decides.
	CHECK(dxr_wl_stage_factor(2.0, 0.0, U_WL_LAYOUT_MODE_UNKNOWN, 3840.0 / 3840.0) == 1.0); // physical
	CHECK(dxr_wl_stage_factor(2.0, 0.0, U_WL_LAYOUT_MODE_UNKNOWN, 3840.0 / 1920.0) == 2.0); // logical
	// Nothing else known: the monitor scale (the pre-v11 behaviour).
	CHECK(dxr_wl_stage_factor(2.0, 0.0, U_WL_LAYOUT_MODE_UNKNOWN, 0.0) == 2.0);
	CHECK(dxr_wl_stage_factor(0.0, 0.0, U_WL_LAYOUT_MODE_UNKNOWN, 0.0) == 0.0);

	if (g_fail != 0) {
		std::printf("linux_window_scale_test: %d failure(s)\n", g_fail);
		return 1;
	}
	std::printf("linux_window_scale_test: OK\n");
	return 0;
}

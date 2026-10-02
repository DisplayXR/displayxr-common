// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The two Wayland scale factors a GNOME window needs, in BOTH of
 *         mutter's layout modes. Pure: unit tested without a compositor
 *         (tests/linux_window_scale_test.cpp).
 *
 * A Wayland window on GNOME deals in three spaces: DEVICE px (the buffer, the
 * panel), SURFACE units (the configure size, wp_viewport destinations) and
 * mutter's STAGE (window positions, the window-geometry extension's rects,
 * xdg_output's logical position and size). Two factors join them:
 *
 *   - device px per SURFACE unit: what a buffer is declared at. The
 *     compositor's preferred scale (wp_fractional_scale_v1) once it arrives,
 *     estimated before then by @ref dxr_wl_surface_scale_estimate.
 *   - device px per STAGE px: what a position or a published rect converts
 *     by, @ref dxr_wl_stage_factor.
 *
 * In mutter's LOGICAL layout mode (fractional scaling; mutter 50's default)
 * the two are the same number, the monitor scale. In its PHYSICAL mode (Ubuntu
 * 24.04 / GNOME 46 at an integer scale, out of the box) the stage IS device px,
 * so the stage factor is 1 while clients still draw at the integer scale.
 * Treating them as one number there sized a 720x400 initial rect at 1440x800,
 * and read a window on a 3840x2160 monitor at 200 % as twice its size.
 * See the runtime's u_wayland_layout.h (vendored in xrt_aux/util).
 */
#pragma once

#include "util/u_wayland_layout.h"

#include <cmath>
#include <cstdint>

/*!
 * Device px per SURFACE unit on an output, before the surface's preferred
 * scale is known.
 *
 * `mode / xdg_output.logical_size` is the STAGE factor: mutter reports the
 * stage size as the logical size in both layout modes. In LOGICAL that is also
 * the surface scale. In PHYSICAL it is 1 while wl_output.scale is the (always
 * integer) scale clients draw at, which is the signature used here: a stage
 * factor of 1 next to an integer scale above 1. (In LOGICAL a scaled output
 * never has a stage factor of 1.)
 *
 * @param int_scale  wl_output.scale for that output (1 when unknown).
 * @return the estimate; 1.0 when nothing is known.
 */
inline double
dxr_wl_surface_scale_estimate(int32_t mode_w, int32_t logical_w, int32_t int_scale)
{
	if (mode_w <= 0 || logical_w <= 0) {
		return 1.0;
	}
	const double stage = (double)mode_w / (double)logical_w;
	if (std::fabs(stage - 1.0) < 1e-3 && int_scale > 1) {
		return (double)int_scale; // PHYSICAL layout
	}
	return stage;
}

/*!
 * Device px per STAGE px for a monitor the window-geometry extension
 * published: the factor its `frame` / `buffer` / `monitor` rects convert by.
 *
 * In order: the extension's own answer (`monitor.device_scale`, or its
 * `layout_mode`; extension version 11), then this client's own output at the
 * monitor's position (`mode / xdg_output.logical_size`, @p output_stage_factor,
 * 0 when there is none), then the monitor scale (what every consumer used
 * before version 11 — right in LOGICAL only).
 */
inline double
dxr_wl_stage_factor(double monitor_scale, double device_scale, enum u_wl_layout_mode layout, double output_stage_factor)
{
	if (device_scale > 0.0 || layout != U_WL_LAYOUT_MODE_UNKNOWN) {
		return u_wl_stage_to_device_scale(monitor_scale, device_scale, layout);
	}
	if (output_stage_factor > 0.0) {
		return output_stage_factor;
	}
	return u_wl_stage_to_device_scale(monitor_scale, 0.0, U_WL_LAYOUT_MODE_UNKNOWN);
}

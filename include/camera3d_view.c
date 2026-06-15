// Copyright 2025-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR-typed wrapper over the type-neutral camera-centric math core
 *
 * The actual math lives in dxr_view_math.c (#396 W7) — this TU is pure
 * pointer-casts. See camera3d_view.h for API docs and display3d_view.c for
 * the layout-compatibility guards (same Xr-type / dxr-type pairs).
 */

#include "camera3d_view.h"
#include "dxr_view_math.h"

#include <assert.h>
#include <stddef.h>

_Static_assert(sizeof(Camera3DTunables) == sizeof(dxr_camera3d_tunables), "tunables layout");
_Static_assert(sizeof(Camera3DView) == sizeof(dxr_camera3d_view), "view layout");
_Static_assert(offsetof(Camera3DView, fov) == offsetof(dxr_camera3d_view, fov), "view layout");
_Static_assert(offsetof(Camera3DView, orientation) == offsetof(dxr_camera3d_view, orientation), "view layout");

Camera3DTunables
camera3d_default_tunables(void)
{
	dxr_camera3d_tunables t = dxr_camera3d_default_tunables();
	Camera3DTunables out;
	out.ipd_factor = t.ipd_factor;
	out.parallax_factor = t.parallax_factor;
	out.inv_convergence_distance = t.inv_convergence_distance;
	out.half_tan_vfov = t.half_tan_vfov;
	out.m2v = t.m2v;
	return out;
}

void
camera3d_compute_view(const XrVector3f *processed_eye,
                      float nominal_z,
                      const Display3DScreen *screen,
                      const Camera3DTunables *tunables,
                      const XrPosef *camera_pose,
                      float near_z,
                      float far_z,
                      Camera3DView *out)
{
	dxr_camera3d_compute_view((const dxr_vec3 *)processed_eye, nominal_z, (const dxr_screen *)screen,
	                          (const dxr_camera3d_tunables *)tunables, (const dxr_pose *)camera_pose, near_z,
	                          far_z, (dxr_camera3d_view *)out);
}

void
camera3d_compute_views(const XrVector3f *raw_eyes,
                       uint32_t count,
                       const XrVector3f *nominal_viewer,
                       const Display3DScreen *screen,
                       const Camera3DTunables *tunables,
                       const XrPosef *camera_pose,
                       float near_z,
                       float far_z,
                       Camera3DView *out_views)
{
	dxr_camera3d_compute_views((const dxr_vec3 *)raw_eyes, count, (const dxr_vec3 *)nominal_viewer,
	                           (const dxr_screen *)screen, (const dxr_camera3d_tunables *)tunables,
	                           (const dxr_pose *)camera_pose, near_z, far_z, (dxr_camera3d_view *)out_views);
}

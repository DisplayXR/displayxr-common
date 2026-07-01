// Copyright 2025-2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  xrt-typed FOV-only wrapper — see displayxr_math_xrt.h
 *
 * Pure pointer-casts over dxr_view_math.c. The clip values passed to the core
 * are benign placeholders: fov / eye_display / eye_world / orientation are
 * clip-independent, and the matrices the core fills are discarded here.
 */

#include "displayxr_math_xrt.h"

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>

// --- Layout-compatibility guards (the casts below rely on these) ------------

_Static_assert(sizeof(struct xrt_vec3) == sizeof(dxr_vec3), "xrt_vec3 / dxr_vec3 layout");
_Static_assert(offsetof(struct xrt_vec3, z) == offsetof(dxr_vec3, z), "xrt_vec3 / dxr_vec3 layout");
_Static_assert(sizeof(struct xrt_quat) == sizeof(dxr_quat), "xrt_quat / dxr_quat layout");
_Static_assert(offsetof(struct xrt_quat, w) == offsetof(dxr_quat, w), "xrt_quat / dxr_quat layout");
_Static_assert(sizeof(struct xrt_pose) == sizeof(dxr_pose), "xrt_pose / dxr_pose layout");
_Static_assert(offsetof(struct xrt_pose, position) == offsetof(dxr_pose, position), "xrt_pose / dxr_pose layout");
_Static_assert(sizeof(struct xrt_fov) == sizeof(dxr_fov), "xrt_fov / dxr_fov layout");
_Static_assert(offsetof(struct xrt_fov, angle_down) == offsetof(dxr_fov, angle_down), "xrt_fov / dxr_fov layout");

void
dxr_xrt_display3d_compute_views(const struct xrt_vec3 *raw_eyes,
                                uint32_t count,
                                const struct xrt_vec3 *nominal_viewer,
                                const dxr_screen *screen,
                                const dxr_display3d_tunables *tunables,
                                const struct xrt_pose *display_pose,
                                struct dxr_xrt_view *out_views)
{
	if (count == 0) {
		return;
	}

	dxr_display3d_view stack_views[8];
	dxr_display3d_view *views =
	    (count <= 8) ? stack_views : (dxr_display3d_view *)malloc(count * sizeof(dxr_display3d_view));

	// Clip placeholders (fov/eye outputs are clip-independent; matrices unused).
	dxr_display3d_compute_views((const dxr_vec3 *)raw_eyes, count, (const dxr_vec3 *)nominal_viewer, screen,
	                            tunables, (const dxr_pose *)display_pose,
	                            /*near_offset=*/0.0f, /*far_offset=*/1.0f,
	                            /*vulkan_flip_y=*/0, views);

	for (uint32_t i = 0; i < count; i++) {
		out_views[i].fov = *(const struct xrt_fov *)&views[i].fov;
		out_views[i].eye_display = *(const struct xrt_vec3 *)&views[i].eye_display;
		out_views[i].eye_world = *(const struct xrt_vec3 *)&views[i].eye_world;
		out_views[i].orientation = *(const struct xrt_quat *)&views[i].orientation;
	}

	if (count > 8) {
		free(views);
	}
}

void
dxr_xrt_camera3d_compute_views(const struct xrt_vec3 *raw_eyes,
                               uint32_t count,
                               const struct xrt_vec3 *nominal_viewer,
                               const dxr_screen *screen,
                               const dxr_camera3d_tunables *tunables,
                               const struct xrt_pose *camera_pose,
                               struct dxr_xrt_view *out_views)
{
	if (count == 0) {
		return;
	}

	dxr_camera3d_view stack_views[8];
	dxr_camera3d_view *views =
	    (count <= 8) ? stack_views : (dxr_camera3d_view *)malloc(count * sizeof(dxr_camera3d_view));

	// Clip placeholders (fov/eye outputs are clip-independent; matrices unused).
	dxr_camera3d_compute_views((const dxr_vec3 *)raw_eyes, count, (const dxr_vec3 *)nominal_viewer, screen,
	                           tunables, (const dxr_pose *)camera_pose,
	                           /*near_z=*/0.01f, /*far_z=*/100.0f, views);

	for (uint32_t i = 0; i < count; i++) {
		out_views[i].fov = *(const struct xrt_fov *)&views[i].fov;
		out_views[i].eye_display = (struct xrt_vec3){0, 0, 0};
		out_views[i].eye_world = *(const struct xrt_vec3 *)&views[i].eye_world;
		out_views[i].orientation = *(const struct xrt_quat *)&views[i].orientation;
	}

	if (count > 8) {
		free(views);
	}
}

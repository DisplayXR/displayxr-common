// Copyright 2025-2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenXR-typed wrapper over the type-neutral multiview math core
 *
 * The actual math lives in dxr_view_math.c (#396 W7) — this TU is pure
 * pointer-casts. The Xr* types and the public structs here are
 * layout-identical to their dxr_* counterparts (static-asserted below), so
 * every wrapper is a cast + call. See display3d_view.h for API docs.
 */

#include "display3d_view.h"
#include "dxr_view_math.h"

#include <assert.h>
#include <stddef.h>

// --- Layout-compatibility guards (the casts below rely on these) ------------

_Static_assert(sizeof(XrVector3f) == sizeof(dxr_vec3), "XrVector3f / dxr_vec3 layout");
_Static_assert(offsetof(XrVector3f, z) == offsetof(dxr_vec3, z), "XrVector3f / dxr_vec3 layout");
_Static_assert(sizeof(XrQuaternionf) == sizeof(dxr_quat), "XrQuaternionf / dxr_quat layout");
_Static_assert(offsetof(XrQuaternionf, w) == offsetof(dxr_quat, w), "XrQuaternionf / dxr_quat layout");
_Static_assert(sizeof(XrPosef) == sizeof(dxr_pose), "XrPosef / dxr_pose layout");
_Static_assert(offsetof(XrPosef, position) == offsetof(dxr_pose, position), "XrPosef / dxr_pose layout");
_Static_assert(sizeof(XrFovf) == sizeof(dxr_fov), "XrFovf / dxr_fov layout");
_Static_assert(offsetof(XrFovf, angleDown) == offsetof(dxr_fov, angle_down), "XrFovf / dxr_fov layout");

_Static_assert(sizeof(Display3DTunables) == sizeof(dxr_display3d_tunables), "tunables layout");
_Static_assert(sizeof(Display3DScreen) == sizeof(dxr_screen), "screen layout");
_Static_assert(sizeof(Display3DWindowPlacement) == sizeof(dxr_window_placement), "placement layout");
_Static_assert(sizeof(Display3DView) == sizeof(dxr_display3d_view), "view layout");
_Static_assert(offsetof(Display3DView, fov) == offsetof(dxr_display3d_view, fov), "view layout");
_Static_assert(offsetof(Display3DView, eye_world) == offsetof(dxr_display3d_view, eye_world), "view layout");
_Static_assert(offsetof(Display3DView, far_z) == offsetof(dxr_display3d_view, far_z), "view layout");

// --- Wrappers ----------------------------------------------------------------

Display3DTunables
display3d_default_tunables(void)
{
	dxr_display3d_tunables t = dxr_display3d_default_tunables();
	Display3DTunables out;
	out.ipd_factor = t.ipd_factor;
	out.parallax_factor = t.parallax_factor;
	out.perspective_factor = t.perspective_factor;
	out.virtual_display_height = t.virtual_display_height;
	return out;
}

void
display3d_resolve_window_rect(const Display3DWindowPlacement *p,
                              const XrVector3f *raw_eyes,
                              uint32_t count,
                              Display3DScreen *out_screen,
                              XrVector3f *out_eyes)
{
	dxr_display3d_resolve_window_rect((const dxr_window_placement *)p, (const dxr_vec3 *)raw_eyes, count,
	                                  (dxr_screen *)out_screen, (dxr_vec3 *)out_eyes);
}

void
display3d_apply_eye_factors(const XrVector3f *raw_left,
                            const XrVector3f *raw_right,
                            const XrVector3f *nominal_viewer,
                            float ipd_factor,
                            float parallax_factor,
                            XrVector3f *out_left,
                            XrVector3f *out_right)
{
	dxr_display3d_apply_eye_factors((const dxr_vec3 *)raw_left, (const dxr_vec3 *)raw_right,
	                                (const dxr_vec3 *)nominal_viewer, ipd_factor, parallax_factor,
	                                (dxr_vec3 *)out_left, (dxr_vec3 *)out_right);
}

void
display3d_apply_eye_factors_n(const XrVector3f *raw_eyes,
                              uint32_t count,
                              const XrVector3f *nominal_viewer,
                              float ipd_factor,
                              float parallax_factor,
                              XrVector3f *out_eyes)
{
	dxr_display3d_apply_eye_factors_n((const dxr_vec3 *)raw_eyes, count, (const dxr_vec3 *)nominal_viewer,
	                                  ipd_factor, parallax_factor, (dxr_vec3 *)out_eyes);
}

XrFovf
display3d_compute_fov(XrVector3f eye_pos, float screen_width_m, float screen_height_m)
{
	dxr_vec3 e = {eye_pos.x, eye_pos.y, eye_pos.z};
	dxr_fov f = dxr_display3d_compute_fov(e, screen_width_m, screen_height_m);
	XrFovf out;
	out.angleLeft = f.angle_left;
	out.angleRight = f.angle_right;
	out.angleUp = f.angle_up;
	out.angleDown = f.angle_down;
	return out;
}

void
display3d_compute_projection(XrVector3f eye_pos,
                             float screen_width_m,
                             float screen_height_m,
                             float near_z,
                             float far_z,
                             float *out_matrix)
{
	dxr_vec3 e = {eye_pos.x, eye_pos.y, eye_pos.z};
	dxr_display3d_compute_projection(e, screen_width_m, screen_height_m, near_z, far_z, out_matrix);
}

void
display3d_compute_view(const XrVector3f *processed_eye,
                       const Display3DScreen *screen,
                       const Display3DTunables *tunables,
                       const XrPosef *display_pose,
                       float near_offset,
                       float far_offset,
                       Display3DView *out)
{
	dxr_display3d_compute_view((const dxr_vec3 *)processed_eye, (const dxr_screen *)screen,
	                           (const dxr_display3d_tunables *)tunables, (const dxr_pose *)display_pose,
	                           near_offset, far_offset, (dxr_display3d_view *)out);
}

void
display3d_compute_views(const XrVector3f *raw_eyes,
                        uint32_t count,
                        const XrVector3f *nominal_viewer,
                        const Display3DScreen *screen,
                        const Display3DTunables *tunables,
                        const XrPosef *display_pose,
                        float near_offset,
                        float far_offset,
                        int vulkan_flip_y,
                        Display3DView *out_views)
{
	dxr_display3d_compute_views((const dxr_vec3 *)raw_eyes, count, (const dxr_vec3 *)nominal_viewer,
	                            (const dxr_screen *)screen, (const dxr_display3d_tunables *)tunables,
	                            (const dxr_pose *)display_pose, near_offset, far_offset, vulkan_flip_y,
	                            (dxr_display3d_view *)out_views);
}

void
display3d_compute_center_view(const XrVector3f *raw_eyes,
                              uint32_t count,
                              const XrVector3f *nominal_viewer,
                              const Display3DScreen *screen,
                              const Display3DTunables *tunables,
                              const XrPosef *display_pose,
                              float near_offset,
                              float far_offset,
                              int vulkan_flip_y,
                              Display3DView *out_view)
{
	dxr_display3d_compute_center_view((const dxr_vec3 *)raw_eyes, count, (const dxr_vec3 *)nominal_viewer,
	                                  (const dxr_screen *)screen, (const dxr_display3d_tunables *)tunables,
	                                  (const dxr_pose *)display_pose, near_offset, far_offset, vulkan_flip_y,
	                                  (dxr_display3d_view *)out_view);
}

int
display3d_selftest(void)
{
	return dxr_display3d_selftest();
}

void
display3d_pose_slerp(const XrPosef *from, const XrPosef *to, float t, XrPosef *out)
{
	dxr_display3d_pose_slerp((const dxr_pose *)from, (const dxr_pose *)to, t, (dxr_pose *)out);
}

void
display3d_align_pose_to_ray(XrVector3f hit_world, XrVector3f ray_dir_world, XrVector3f up_hint, XrPosef *out)
{
	dxr_vec3 hit = {hit_world.x, hit_world.y, hit_world.z};
	dxr_vec3 dir = {ray_dir_world.x, ray_dir_world.y, ray_dir_world.z};
	dxr_vec3 up = {up_hint.x, up_hint.y, up_hint.z};
	dxr_display3d_align_pose_to_ray(hit, dir, up, (dxr_pose *)out);
}

void
display3d_unproject_ndc_to_ray(float ndc_x,
                               float ndc_y,
                               const float *view_col_major,
                               const float *proj_col_major,
                               XrVector3f *out_origin_world,
                               XrVector3f *out_dir_world)
{
	dxr_display3d_unproject_ndc_to_ray(ndc_x, ndc_y, view_col_major, proj_col_major,
	                                   (dxr_vec3 *)out_origin_world, (dxr_vec3 *)out_dir_world);
}

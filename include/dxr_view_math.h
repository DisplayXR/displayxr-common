// Copyright 2025-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Type-neutral core of the DisplayXR multiview math (#396 W7)
 *
 * The ONE implementation of the Kooima display-centric and camera-centric
 * rig math, typed on its own minimal POD types (dxr_vec3 / dxr_quat /
 * dxr_pose / dxr_fov). Zero dependencies beyond <math.h> — in particular
 * NO OpenXR headers and NO runtime (xrt) headers.
 *
 * Two thin typed wrappers sit on top, both pure pointer-casts (the dxr_*
 * types are layout-identical to their counterparts on both sides):
 *
 *   - display3d_view.h / camera3d_view.h — the OpenXR-typed app-side API
 *     (XrVector3f / XrFovf / XrPosef), unchanged for the 5 pinned consumers.
 *   - displayxr_math_xrt.h — the xrt-typed FOV-only API for the DisplayXR
 *     runtime (xrt_vec3 / xrt_fov / xrt_pose), replacing the runtime's
 *     hand-synced m_display3d_view / m_camera3d_view / m_multiview ports.
 *
 * Because both wrappers run THIS code, "runtime render-ready output" and
 * "app-computed-from-raw output" are the same function by construction —
 * the equivalence guarantee of XR_EXT_view_rig
 * (docs/roadmap/raw-vs-render-ready-views.md in displayxr-runtime).
 *
 * Reference: Robert Kooima, "Generalized Perspective Projection" (2009).
 * Matrix convention: all output matrices are column-major (OpenGL/Vulkan/
 * Metal); GL [-1,1] clip depth (consumers remap via projection_depth.h).
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Types -----------------------------------------------------------------
// Layout-identical to XrVector3f/XrQuaternionf/XrPosef/XrFovf AND to
// xrt_vec3/xrt_quat/xrt_pose/xrt_fov. The wrappers static-assert this.

typedef struct dxr_vec3 {
	float x, y, z;
} dxr_vec3;

typedef struct dxr_quat {
	float x, y, z, w;
} dxr_quat;

typedef struct dxr_pose {
	dxr_quat orientation;
	dxr_vec3 position;
} dxr_pose;

//! Asymmetric FOV half-angles in radians, signed (left/down negative).
//! Field order matches XrFovf (angleLeft, angleRight, angleUp, angleDown)
//! and xrt_fov (angle_left, angle_right, angle_up, angle_down).
typedef struct dxr_fov {
	float angle_left, angle_right, angle_up, angle_down;
} dxr_fov;

// --- Structs (mirror the OpenXR-typed API one-for-one) ----------------------

typedef struct dxr_display3d_tunables {
	float ipd_factor;             //!< [0, 1] — scales inter-eye distance (0=mono, 1=full)
	float parallax_factor;        //!< [0, 1] — lerps eye center toward nominal (0=no tracking, 1=full)
	float perspective_factor;     //!< [0.1, 10] — scales eye XYZ only (changes object perspective)
	float virtual_display_height; //!< Virtual display height in app units (always required)
} dxr_display3d_tunables;

typedef struct dxr_screen {
	float width_m;  //!< Physical screen width (meters)
	float height_m; //!< Physical screen height (meters)
} dxr_screen;

typedef struct dxr_window_placement {
	float display_width_m;   //!< Physical display width (meters)
	float display_height_m;  //!< Physical display height (meters)
	float display_width_px;  //!< Display resolution width (pixels)
	float display_height_px; //!< Display resolution height (pixels)
	float rect_center_x_px;  //!< Window/canvas CENTER on the display, X (pixels; origin top-left, Y-down)
	float rect_center_y_px;  //!< Window/canvas CENTER on the display, Y (pixels, Y-down)
	float rect_width_px;     //!< Rect width (pixels)
	float rect_height_px;    //!< Rect height (pixels)
} dxr_window_placement;

typedef struct dxr_display3d_view {
	float view_matrix[16];       //!< Column-major 4x4 view matrix
	float projection_matrix[16]; //!< Column-major 4x4 projection matrix
	dxr_fov fov;                 //!< Equivalent asymmetric FOV angles (radians)
	dxr_vec3 eye_display;        //!< Modified eye position in display space (after all factors)
	dxr_vec3 eye_world;          //!< Eye position in world space (after display pose transform)
	dxr_quat orientation;        //!< Display orientation (same for all views)
	float near_z;                //!< Resolved view-space near plane = ez - near_offset (clamped)
	float far_z;                 //!< Resolved view-space far plane  = ez + far_offset (clamped)
} dxr_display3d_view;

typedef struct dxr_camera3d_tunables {
	float ipd_factor;               //!< [0, 1] — scales inter-eye distance (0=mono, 1=full)
	float parallax_factor;          //!< [0, 1] — lerps eye center toward nominal (0=no tracking, 1=full)
	float inv_convergence_distance; //!< 1/convergence_dist (1/meters)
	float half_tan_vfov;            //!< tan(vFOV/2) — divide by zoom at call site
} dxr_camera3d_tunables;

typedef struct dxr_camera3d_view {
	float view_matrix[16];       //!< Column-major 4x4 (per-eye: eye displaced in world space)
	float projection_matrix[16]; //!< Column-major 4x4 asymmetric frustum (per-eye)
	dxr_fov fov;                 //!< Asymmetric FOV angles in radians (per-eye)
	dxr_vec3 eye_world;          //!< Eye position in world space
	dxr_quat orientation;        //!< Camera orientation (same for all views)
} dxr_camera3d_view;

// --- Display-centric rig -----------------------------------------------------
// Semantics documented on the OpenXR-typed API (display3d_view.h); these are
// the same functions one cast away.

dxr_display3d_tunables
dxr_display3d_default_tunables(void);

void
dxr_display3d_resolve_window_rect(const dxr_window_placement *placement,
                                  const dxr_vec3 *raw_eyes,
                                  uint32_t count,
                                  dxr_screen *out_screen,
                                  dxr_vec3 *out_eyes);

void
dxr_display3d_apply_eye_factors(const dxr_vec3 *raw_left,
                                const dxr_vec3 *raw_right,
                                const dxr_vec3 *nominal_viewer,
                                float ipd_factor,
                                float parallax_factor,
                                dxr_vec3 *out_left,
                                dxr_vec3 *out_right);

void
dxr_display3d_apply_eye_factors_n(const dxr_vec3 *raw_eyes,
                                  uint32_t count,
                                  const dxr_vec3 *nominal_viewer,
                                  float ipd_factor,
                                  float parallax_factor,
                                  dxr_vec3 *out_eyes);

dxr_fov
dxr_display3d_compute_fov(dxr_vec3 eye_pos, float screen_width_m, float screen_height_m);

void
dxr_display3d_compute_projection(dxr_vec3 eye_pos,
                                 float screen_width_m,
                                 float screen_height_m,
                                 float near_z,
                                 float far_z,
                                 float *out_matrix);

void
dxr_display3d_compute_view(const dxr_vec3 *processed_eye,
                           const dxr_screen *screen,
                           const dxr_display3d_tunables *tunables,
                           const dxr_pose *display_pose,
                           float near_offset,
                           float far_offset,
                           dxr_display3d_view *out);

void
dxr_display3d_compute_views(const dxr_vec3 *raw_eyes,
                            uint32_t count,
                            const dxr_vec3 *nominal_viewer,
                            const dxr_screen *screen,
                            const dxr_display3d_tunables *tunables,
                            const dxr_pose *display_pose,
                            float near_offset,
                            float far_offset,
                            int vulkan_flip_y,
                            dxr_display3d_view *out_views);

void
dxr_display3d_compute_center_view(const dxr_vec3 *raw_eyes,
                                  uint32_t count,
                                  const dxr_vec3 *nominal_viewer,
                                  const dxr_screen *screen,
                                  const dxr_display3d_tunables *tunables,
                                  const dxr_pose *display_pose,
                                  float near_offset,
                                  float far_offset,
                                  int vulkan_flip_y,
                                  dxr_display3d_view *out_view);

int
dxr_display3d_selftest(void);

void
dxr_display3d_pose_slerp(const dxr_pose *from, const dxr_pose *to, float t, dxr_pose *out);

void
dxr_display3d_align_pose_to_ray(dxr_vec3 hit_world, dxr_vec3 ray_dir_world, dxr_vec3 up_hint, dxr_pose *out);

void
dxr_display3d_unproject_ndc_to_ray(float ndc_x,
                                   float ndc_y,
                                   const float *view_col_major,
                                   const float *proj_col_major,
                                   dxr_vec3 *out_origin_world,
                                   dxr_vec3 *out_dir_world);

// --- Camera-centric rig ------------------------------------------------------

dxr_camera3d_tunables
dxr_camera3d_default_tunables(void);

void
dxr_camera3d_compute_view(const dxr_vec3 *processed_eye,
                          float nominal_z,
                          const dxr_screen *screen,
                          const dxr_camera3d_tunables *tunables,
                          const dxr_pose *camera_pose,
                          float near_z,
                          float far_z,
                          dxr_camera3d_view *out);

void
dxr_camera3d_compute_views(const dxr_vec3 *raw_eyes,
                           uint32_t count,
                           const dxr_vec3 *nominal_viewer,
                           const dxr_screen *screen,
                           const dxr_camera3d_tunables *tunables,
                           const dxr_pose *camera_pose,
                           float near_z,
                           float far_z,
                           dxr_camera3d_view *out_views);

#ifdef __cplusplus
}
#endif

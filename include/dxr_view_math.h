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
	float inv_convergence_distance; //!< 1/convergence_dist, in 1/world-units (0 = infinity)
	float half_tan_vfov;            //!< tan(vFOV/2) — divide by zoom at call site
	float m2v;                      //!< meters→world scale on the eye (default 1; <=0 treated as 1)
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

// --- Rig equivalence / conversion -------------------------------------------
//
// The display-centric and camera-centric rigs are two parameterizations of one
// off-axis view state. The camera rig is a strict superset by one DOF (its
// convergence is free; the display rig pins convergence to the display plane,
// i.e. the nominal viewing distance). These converters let an app switch rig
// type with no visual disturbance and then animate the new rig's parameters.

// Physical display facts (straight from XR_EXT_display_info).
typedef struct dxr_rig_display_info {
	float physical_height_m;  //!< physical display/canvas height (meters)
	float aspect;             //!< width / height
	float nominal_distance_m; //!< nominal viewing distance (meters)
} dxr_rig_display_info;

// Full display-rig description (params + pose), mirrors XrDisplayRigEXT.
typedef struct dxr_display_rig {
	dxr_pose pose;                //!< virtual display plane pose
	float virtual_display_height; //!< app/world units
	float ipd_factor;
	float parallax_factor;
	float perspective_factor;
} dxr_display_rig;

// Full camera-rig description (params + pose), mirrors XrCameraRigEXT.
typedef struct dxr_camera_rig {
	dxr_pose pose;                  //!< camera pose
	float ipd_factor;
	float parallax_factor;
	float inv_convergence_distance; //!< 1/world-units (0 = infinity)
	float half_tan_vfov;            //!< tan(vFOV/2)
	float m2v;                      //!< meters→world scale
} dxr_camera_rig;

// Display → camera. Always exact (the camera rig is a superset).
void
dxr_view_rig_display_to_camera(const dxr_display_rig *in, const dxr_rig_display_info *info, dxr_camera_rig *out);

// Camera → display. Exact for ANY convergence: vH carries the convergence
// distance and the ipd/parallax factors are rescaled by f = m2v*invd*N so the
// per-eye disparity matches too (the display rig couples convergence and eye-
// scale through one factor; ipd supplies the missing DOF). The reverse emits
// m2v = es which re-absorbs f, so display<->camera round-trips render
// identically. Caveat: f > 1 (convergence nearer than nominal) yields
// ipd_factor > 1; a caller that clamps ipd to [0,1] will fall short of exact
// there — far convergence (f <= 1) is always exact. Returns the convergence
// delta from the compatible value (informational, not a lossy flag).
float
dxr_view_rig_camera_to_display(const dxr_camera_rig *in, const dxr_rig_display_info *info, dxr_display_rig *out);

// --- Rig transition (convert-then-lerp) -------------------------------------
//
// A tagged rig: either parameterization. The transition helper lerps between
// two of these over a normalized parameter t; if the types differ it does the
// exact (disturbance-free) type conversion first, then lerps in the target
// type's space. PURE — the caller owns time + easing (so it is testable and
// interruptible: snapshot the current eval as a new `from` and restart t).

typedef enum dxr_rig_type {
	DXR_RIG_DISPLAY = 0,
	DXR_RIG_CAMERA = 1,
} dxr_rig_type;

typedef struct dxr_rig {
	dxr_rig_type type;
	union {
		dxr_display_rig display;
		dxr_camera_rig camera;
	} u;
} dxr_rig;

// out->type == to->type. If from->type != to->type, `from` is converted into
// to->type FIRST (exact converters → t<=0 is disturbance-free), then each
// parameter is lerped in its chosen space: inv_convergence_distance linear (in
// diopters — sails through infinite convergence); virtual_display_height &
// perspective_factor log/multiplicative; FOV lerped in angle; ipd/parallax
// linear; pose = slerp orientation + linear position. t<=0 emits the converted
// `from`; t>=1 copies `to`. When the output is camera-type its convergence is
// clamped for stereo comfort (see dxr_rig_clamp_for_comfort) using comfort_far_z
// (<=0 → conservative infinity clamp). `t` is raw [0,1]; easing is the caller's.
void
dxr_rig_transition(const dxr_rig *from,
                   const dxr_rig *to,
                   const dxr_rig_display_info *info,
                   float t,
                   float comfort_far_z,
                   dxr_rig *out);

// --- Stereo comfort -----------------------------------------------------------
//
// The display-equivalent ipd_factor is the canonical comfort metric: it bounds
// the screen disparity of far content. comfort_factor = ipd_factor * f, with
// f = m2v * invd * N. <= 1 comfortable; == 1 divergence limit (infinity exactly
// at parallel); > 1 the eyes DIVERGE for far content (uncomfortable/unsafe).
// Only ipd matters here — parallax (head-tracking response) and vHeight
// (framing) do not change infinity disparity.
float
dxr_rig_comfort_factor(const dxr_camera_rig *cam, const dxr_rig_display_info *info);

// Maximum comfortable convergence (1/world-units), enforcing comfort_factor <= 1
// via the convergence knob (never by clamping ipd). far_z <= 0 → the conservative
// infinity case 1/(max(1,ipd)*m2v*N): for ipd<=1 that is 1/(m2v*N) (zero-parallax
// no nearer than nominal, also the converter's exact f<=1 regime); for ipd>1 the
// binding term 1/(ipd*m2v*N). A depth-aware caller passes its scene far plane
// (world distance from the eye) to allow converging nearer (far-content disparity
// scales with 1/far_z, so a shallow scene has headroom).
float
dxr_rig_max_convergence(const dxr_camera_rig *cam, const dxr_rig_display_info *info, float far_z);

// Maximum camera ipd_factor whose DISPLAY-centric equivalent is exactly 1, at the
// rig's CURRENT convergence: M = 1/f, f = m2v*invd*N. The dual of
// dxr_rig_max_convergence (which bounds invd at fixed ipd); this bounds ipd at
// fixed convergence. The display rig clamps ipd to [0,1]; the camera rig's
// comfortable ceiling is this M — >1 for far convergence (f<1), <1 for near (f>1).
// cam->ipd_factor is NOT read. f<=0 (convergence at/behind infinity, or degenerate
// params) → no ceiling → returns +infinity (HUGE_VALF). Use when a caller bounds an
// IPD knob at fixed convergence; the library's own comfort enforcement clamps
// convergence instead (dxr_rig_clamp_for_comfort), never ipd.
float
dxr_rig_max_ipd_factor(const dxr_camera_rig *cam, const dxr_rig_display_info *info);

// Clamp convergence for comfort in place: invd = min(invd, max_convergence).
// Enforce comfort HERE (at the convergence knob), never by clamping ipd_factor —
// comfort is the product ipd*m2v*invd*N, so a rig with ipd>1 but tiny invd is
// perfectly comfortable and clamping its ipd would wrongly distort it.
void
dxr_rig_clamp_for_comfort(dxr_camera_rig *cam, const dxr_rig_display_info *info, float far_z);

#ifdef __cplusplus
}
#endif

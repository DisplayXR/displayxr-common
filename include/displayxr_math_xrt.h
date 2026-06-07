// Copyright 2025-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  xrt-typed FOV-only wrapper over the type-neutral multiview math core
 *
 * The DisplayXR runtime's view of the shared Kooima math (#396 W7): replaces
 * the runtime's hand-synced m_display3d_view / m_camera3d_view / m_multiview
 * ports. FOV-only — the runtime extracts {fov, eye_world} per view and builds
 * no matrices, and fov/eye_world are clip-independent, so this API carries no
 * clip parameters and no matrix outputs.
 *
 * Zero OpenXR dependency. Compiled only when the consumer provides the xrt
 * core headers (CMake: DISPLAYXR_XRT_INCLUDE_DIR = the directory containing
 * xrt/xrt_defines.h) — i.e. by the runtime build, which CI-compiles it on
 * every PR. The tunables / screen structs are the core's own (pure floats);
 * only the vector/pose/fov types at the boundary are xrt-typed, and they are
 * layout-identical to the dxr_* core types (static-asserted in the .c).
 *
 * Because this and the OpenXR-typed app wrapper run the SAME core
 * (dxr_view_math.c), runtime render-ready output and app-computed-from-raw
 * output are the same function by construction — the XR_EXT_view_rig
 * equivalence guarantee.
 */

#pragma once

#include "dxr_view_math.h"

#include "xrt/xrt_defines.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * FOV-only per-view output — mirrors what the runtime consumed from its
 * former Display3DView/Camera3DView ports (matrices dropped).
 */
struct dxr_xrt_view
{
	struct xrt_fov fov;          //!< Asymmetric FOV angles (radians), signed
	struct xrt_vec3 eye_display; //!< Display path: eye in display space after all factors; camera path: zero
	struct xrt_vec3 eye_world;   //!< Eye position in world space (after pose transform)
	struct xrt_quat orientation; //!< Display/camera orientation (same for all views)
};

/*!
 * Display-centric rig (Kooima): the window/canvas is a portal into the scene.
 * Same pipeline as the OpenXR-typed display3d_compute_views, minus clip and
 * matrices. vulkan_flip_y is fixed at 0 (the runtime works in the clean
 * +Y-up frame; render-API depth/Y conventions are app-side).
 *
 * @param raw_eyes       N raw eye positions in DISPLAY space
 * @param count          Number of views (>= 1)
 * @param nominal_viewer Nominal viewer in DISPLAY space (or NULL for {0,0,0.5})
 * @param screen         Physical screen dimensions (meters)
 * @param tunables       Display rig tunables (or NULL for defaults)
 * @param display_pose   Display pose in world space (or NULL for identity)
 * @param out_views      Output array of N FOV-only views
 */
void
dxr_xrt_display3d_compute_views(const struct xrt_vec3 *raw_eyes,
                                uint32_t count,
                                const struct xrt_vec3 *nominal_viewer,
                                const dxr_screen *screen,
                                const dxr_display3d_tunables *tunables,
                                const struct xrt_pose *display_pose,
                                struct dxr_xrt_view *out_views);

/*!
 * Camera-centric rig: an app camera whose frustum eye tracking perturbs.
 * Same pipeline as the OpenXR-typed camera3d_compute_views, minus clip and
 * matrices.
 *
 * @param raw_eyes       N raw eye positions in DISPLAY space
 * @param count          Number of views (>= 1)
 * @param nominal_viewer Nominal viewer in DISPLAY space (or NULL for {0,0,0.5})
 * @param screen         Physical screen dimensions (used for aspect ratio)
 * @param tunables       Camera rig tunables (or NULL for defaults)
 * @param camera_pose    Camera pose in world space (or NULL for identity)
 * @param out_views      Output array of N FOV-only views (eye_display = zero)
 */
void
dxr_xrt_camera3d_compute_views(const struct xrt_vec3 *raw_eyes,
                               uint32_t count,
                               const struct xrt_vec3 *nominal_viewer,
                               const dxr_screen *screen,
                               const dxr_camera3d_tunables *tunables,
                               const struct xrt_pose *camera_pose,
                               struct dxr_xrt_view *out_views);

// Tunables defaults: use the core's dxr_display3d_default_tunables() /
// dxr_camera3d_default_tunables() directly — the structs are the core's own.

#ifdef __cplusplus
}
#endif

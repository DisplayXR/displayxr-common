// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Disturbance-free display<->camera rig toggle + absolute reset.
 *
 * Platform-neutral (no <windows.h>, no DirectXMath): the Windows input handler
 * (common/input_handler.cpp) and each macOS test app (.mm, bespoke input) call
 * these so the C-toggle / SPACE-reset behavior is one shared implementation.
 * The math itself lives in displayxr-common's dxr_view_rig_* converters.
 */
#pragma once

#include "view_params.h"

namespace dxr {

//! Disturbance-free rig round-trip toggle (C key). Converts the CURRENT rig
//! (vp + cameraMode + pose) into its exact equivalent in the other
//! parameterization, so the rendered view is unchanged across the toggle.
//! display->camera->display is an identity → repeated toggles never move the
//! scene. The orientation is preserved (caller keeps its yaw/pitch); only the
//! position is rewritten.
//!
//!  vp,cameraMode : in/out rig description (mutated in place)
//!  pos           : in/out rig position {x,y,z}
//!  quat          : in rig orientation {x,y,z,w} — pass the SAME quaternion the
//!                  app submits to the runtime so the convention matches
//!  canvasWidthM,
//!  canvasHeightM : window client area in meters the runtime renders into. The
//!                  rig math runs on the CANVAS, not the full display, so a
//!                  windowed app must pass these (0 → fall back to fallbackHeightM).
//!  nominalZ      : nominal viewer distance (m)
//!  fallbackHeightM : full display height (m), used when canvasHeightM <= 0
void RigToggleMode(ViewParams &vp,
                   bool &cameraMode,
                   float pos[3],
                   const float quat[4],
                   float canvasWidthM,
                   float canvasHeightM,
                   float nominalZ,
                   float fallbackHeightM);

//! Absolute reset to the initial DISPLAY-centric state: display rig, pose at
//! origin / identity, the given initial vHeight, every other tunable at its
//! default (incl. cameraM2v = 1). Dumb and absolute — no conversion, no
//! dependence on the current rig — so there is no room for drift.
//!
//!  initialVHeight : the app's startup virtual display height; <= 0 keeps the
//!                   current vHeight (pre-absolute-reset back-compat).
void RigResetToInitial(ViewParams &vp,
                       bool &cameraMode,
                       float pos[3],
                       float &yaw,
                       float &pitch,
                       float initialVHeight);

} // namespace dxr

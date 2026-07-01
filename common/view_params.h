// Copyright 2025, The DisplayXR Project and its contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  View parameters struct for 3D display rendering
 *
 * UI-facing struct for IPD, parallax, perspective, and scale factors.
 * The math that applies these factors lives in display3d_view.h.
 */

#pragma once

struct ViewParams {
    float ipdFactor = 1.0f;             // [0, 1] — 0=mono, 1=normal IPD. Per-frame
                                        // RENDER output: the ModeSwitch sequencer
                                        // writes the ramped disparity here each
                                        // frame; equals steadyIpdFactor when idle.
    float steadyIpdFactor = 1.0f;       // [0, 1] — user-tuned full-3D target. The
                                        // +/- keys and shift+wheel edit THIS (and the
                                        // camera ceiling clamp clips THIS); ModeSwitch
                                        // ramps ipdFactor toward it on a 2D↔3D switch.
    float parallaxFactor = 1.0f;        // [0, 1] — 0=no head tracking, 1=full

    // Display-centric mode
    float perspectiveFactor = 1.0f;     // [0.1, 10] — scales eye in Kooima only
    float scaleFactor = 1.0f;           // [0.1, 10] — zoom via vHeight/scale
    float virtualDisplayHeight = 0.0f;  // virtual display height in app units (0 = disabled, 1:1 meters)

    // Camera-centric mode
    float invConvergenceDistance = 0.5f; // [0.1, 10] — 1/convergence_dist (default 0.5 = 2m)
    float zoomFactor = 1.0f;            // [0.1, 10] — divides half_tan_vfov
    float cameraM2v = 1.0f;             // meters→world scale (XrCameraRigEXT::metersToVirtual);
                                        // derived by the C-toggle converter so the camera rig
                                        // reproduces the display rig's eye scaling exactly
};

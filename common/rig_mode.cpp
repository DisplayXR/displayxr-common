// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Implementation of the shared rig toggle + reset — see rig_mode.h.
 */
#include "rig_mode.h"
#include "dxr_view_math.h" // dxr_view_rig_display_to_camera / camera_to_display

namespace dxr {

// Reference camera vertical-FOV half-tangent (tan(18°) → 36° vFOV). The toggle
// maps zoomFactor <-> camera half_tan_vfov through this; apps submit
// XrCameraRigEXT::verticalFov = 2*atan(this / zoomFactor) with the same value.
static const float kCameraHalfTanVfov = 0.32491969623f;

void
RigToggleMode(ViewParams &vp,
              bool &cameraMode,
              float pos[3],
              const float quat[4],
              float canvasWidthM,
              float canvasHeightM,
              float nominalZ,
              float fallbackHeightM)
{
	// The rig math runs on the canvas (window client area), NOT the full
	// display — in a window the window "is" the display.
	const float H = (canvasHeightM > 0.0f) ? canvasHeightM : fallbackHeightM;
	const float N = nominalZ;
	if (H <= 0.0f || N <= 0.0f) {
		cameraMode = !cameraMode; // no display info yet — plain flip so C still responds
		return;
	}

	dxr_rig_display_info info;
	info.physical_height_m = H;
	info.aspect = (canvasWidthM > 0.0f) ? (canvasWidthM / H) : 1.0f; // unused by the converters
	info.nominal_distance_m = N;

	const dxr_quat q = {quat[0], quat[1], quat[2], quat[3]};
	const dxr_vec3 p = {pos[0], pos[1], pos[2]};

	if (!cameraMode) {
		// Display -> camera.
		dxr_display_rig disp;
		disp.pose.orientation = q;
		disp.pose.position = p;
		disp.virtual_display_height = vp.virtualDisplayHeight / vp.scaleFactor;
		disp.ipd_factor = vp.ipdFactor;
		disp.parallax_factor = vp.parallaxFactor;
		disp.perspective_factor = vp.perspectiveFactor;

		dxr_camera_rig cam;
		dxr_view_rig_display_to_camera(&disp, &info, &cam);

		vp.invConvergenceDistance = cam.inv_convergence_distance;
		vp.zoomFactor = kCameraHalfTanVfov / cam.half_tan_vfov;
		vp.cameraM2v = cam.m2v;
		vp.ipdFactor = cam.ipd_factor;
		vp.parallaxFactor = cam.parallax_factor;
		pos[0] = cam.pose.position.x;
		pos[1] = cam.pose.position.y;
		pos[2] = cam.pose.position.z;
		cameraMode = true;
	} else {
		// Camera -> display.
		dxr_camera_rig cam;
		cam.pose.orientation = q;
		cam.pose.position = p;
		cam.ipd_factor = vp.ipdFactor;
		cam.parallax_factor = vp.parallaxFactor;
		cam.inv_convergence_distance = vp.invConvergenceDistance;
		cam.half_tan_vfov = kCameraHalfTanVfov / vp.zoomFactor;
		cam.m2v = vp.cameraM2v;

		dxr_display_rig disp;
		dxr_view_rig_camera_to_display(&cam, &info, &disp);

		// Preserve the user's display-mode scaleFactor (the rig submits
		// virtualDisplayHeight / scaleFactor).
		vp.virtualDisplayHeight = disp.virtual_display_height * vp.scaleFactor;
		vp.perspectiveFactor = disp.perspective_factor;
		vp.ipdFactor = disp.ipd_factor;
		vp.parallaxFactor = disp.parallax_factor;
		pos[0] = disp.pose.position.x;
		pos[1] = disp.pose.position.y;
		pos[2] = disp.pose.position.z;
		cameraMode = false;
	}
}

void
RigResetToInitial(ViewParams &vp, bool &cameraMode, float pos[3], float &yaw, float &pitch, float initialVHeight)
{
	const float vh = (initialVHeight > 0.0f) ? initialVHeight : vp.virtualDisplayHeight;
	cameraMode = false; // display-centric
	pos[0] = 0.0f;      // pose at origin
	pos[1] = 0.0f;
	pos[2] = 0.0f;
	yaw = 0.0f; // identity orientation
	pitch = 0.0f;
	vp = ViewParams{}; // ipd/parallax/perspective/scale = 1, conv = 0.5, zoom = 1, m2v = 1
	vp.virtualDisplayHeight = vh;
}

} // namespace dxr

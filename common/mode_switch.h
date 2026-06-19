// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Smooth 2D<->3D rendering-mode switch sequencer.
 *
 * A platform-neutral state machine that eases the stereo disparity around a
 * rendering-mode switch by ramping the view-rig `ipdFactor`, and — crucially —
 * owns the *sequencing asymmetry* that hand-rolled versions get wrong:
 *
 *   - 3D -> 2D : ramp the disparity to 0 FIRST, then issue the mode request, so
 *                2D lands on already-flat content.
 *   - 2D -> 3D : issue the mode request FIRST (the first 3D frame is flat), then
 *                ease the disparity up to the app's steady value.
 *
 * The runtime's one-frame eye-set coherence guard (issue #615) keeps the very
 * first 3D frame from snapping; this helper is the multi-frame aesthetic ramp on
 * top. Driven by wall-clock dt (frame-rate independent), interruptible (mashing
 * the toggle mid-ramp reverses cleanly — a not-yet-fired ->2D simply ramps back
 * up without ever switching), and dependency-free (own easing, no dxr_view_math
 * / OpenXR / windows.h) so the native apps, the demos, and the engine ports can
 * all share one implementation.
 *
 * Usage (per frame, in the app's main loop):
 * @code
 *   // On the toggle key, compute the target mode and hand it to the sequencer.
 *   if (cycleRequested) {
 *       uint32_t next = (xr.currentModeIndex + 1) % xr.renderingModeCount;
 *       mode_switch.request(next, viewCountOf(next),
 *                           xr.currentModeIndex, viewCountOf(xr.currentModeIndex),
 *                           viewParams.ipdFactor, app.steadyIpd);
 *   }
 *   // Every frame: advance the ramp and act on its outputs.
 *   float ipd; bool fire; uint32_t mode;
 *   mode_switch.update(dt_seconds, &ipd, &fire, &mode);
 *   viewParams.ipdFactor = ipd;                 // submit on the rig this frame
 *   if (fire && mode != xr.currentModeIndex)    // issue the runtime switch
 *       pfnRequestDisplayRenderingModeEXT(session, mode);
 * @endcode
 */
#pragma once

#include <cstdint>

namespace dxr {

//! Easing curve for the disparity ramp. Mirrors dxr::Easing (rig_transition.h)
//! but is redeclared here so this sequencer stays dependency-free for the
//! engine ports — it deliberately pulls in no dxr_view_math / OpenXR headers.
enum class ModeSwitchEasing {
	Linear,
	SmoothStep,  //!< Hermite 3t^2-2t^3 (ease in/out) — default
	EaseOutCubic //!< 1-(1-t)^3
};

//! Smooth 2D<->3D mode-switch sequencer. One instance per session.
class ModeSwitch
{
public:
	//! Ramp duration (seconds) and easing curve. duration_s <= 0 → instant
	//! (fire immediately, no ramp). Defaults: 0.18 s, SmoothStep.
	void configure(float duration_s, ModeSwitchEasing easing = ModeSwitchEasing::SmoothStep);

	//! Begin — or, if called mid-flight, seamlessly redirect — a switch to
	//! `targetMode`. Safe to call every time the user toggles; a mid-ramp call
	//! acts as a retarget from the current disparity (pass `currentIpd` = the
	//! last value update() returned).
	//!
	//!  targetMode    : the rendering-mode index the user wants
	//!  targetVC      : that mode's active view count (1 = 2D/mono, >1 = 3D)
	//!  currentMode   : the runtime's CURRENT mode index (xr.currentModeIndex)
	//!  currentVC     : the current mode's active view count
	//!  currentIpd    : the ipdFactor in effect right now (last update() output,
	//!                  or the app's steady ipd when idle)
	//!  steadyIpd     : the app's full-3D ipdFactor to restore to (e.g. the
	//!                  configured viewParams.ipdFactor; NOT assumed to be 1.0 —
	//!                  the user may have tuned it, or a camera rig caps it)
	void request(uint32_t targetMode,
	             uint32_t targetVC,
	             uint32_t currentMode,
	             uint32_t currentVC,
	             float currentIpd,
	             float steadyIpd);

	//! Advance the ramp by dt_s and report this frame's outputs.
	//!  *outIpd       : the ipdFactor to submit on the rig this frame
	//!  *outFire      : true on exactly ONE frame — when the caller should issue
	//!                  pfnRequestDisplayRenderingModeEXT(*outMode). The caller
	//!                  should still skip the call when *outMode == currentMode
	//!                  (a no-op reversal), though the runtime tolerates it.
	//!  *outMode      : the mode index to request when *outFire is true
	//! Any of the out-pointers may be null.
	void update(float dt_s, float *outIpd, bool *outFire, uint32_t *outMode);

	//! True while a ramp is in flight or a mode request is still pending.
	bool active() const { return phase_ != Phase::Idle; }

	//! Current ipdFactor without advancing the clock.
	float ipd() const { return cur_; }

private:
	enum class Phase {
		Idle,
		RampDownThenFire, //!< 3D->2D: ramp disparity to 0, THEN fire the switch
		FireThenRampUp    //!< ->3D or same-dim: fire now, THEN ease disparity
	};

	float ease(float t) const;

	Phase phase_ = Phase::Idle;
	uint32_t targetMode_ = 0;
	bool firePending_ = false; //!< FireThenRampUp: emit fire on the next update
	bool fireAtEnd_ = false;   //!< RampDownThenFire: emit fire when the ramp lands

	float from_ = 0.0f;
	float to_ = 0.0f;
	float cur_ = 0.0f; //!< last evaluated ipd
	float t_ = 1.0f;   //!< normalized progress; 1 = landed/idle
	float dur_ = 0.18f;
	ModeSwitchEasing easing_ = ModeSwitchEasing::SmoothStep;
};

} // namespace dxr

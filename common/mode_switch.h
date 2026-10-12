// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Smooth 2D<->3D rendering-mode switch sequencer (header-only).
 *
 * THE 2D<->3D TRANSITION CONTRACT (see README "Smooth 2D<->3D transitions"):
 * the runtime switches a rendering mode in ONE frame — the atlas recipe, the
 * display processor's weave-vs-flat choice and the switchable lens all follow
 * the mode the moment it lands (ADR-028). The runtime does NOT ease the
 * stereo disparity: that is app content, and the app owns it. So an app that
 * calls xrRequestDisplayRenderingModeDXR directly on a key press snaps between
 * full parallax and flat. Every app — every platform, every graphics API —
 * should route its mode requests through this sequencer instead.
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
 * / OpenXR / windows.h / AppKit) and HEADER-ONLY, so the native apps on every
 * platform, the demos, the Android legs (via displayxr::rules) and the engine
 * ports can all share one implementation.
 *
 * Usage — the one-call form every app's main loop should use (any platform):
 * @code
 *   static dxr::ModeSwitch g_modeSwitch;   // defaults: 0.18 s, SmoothStep
 *
 *   // Each frame, BEFORE xrLocateViews:
 *   int32_t want = -1;                                         // no request
 *   if (vPressed)      want = dxr::ModeSwitch::cycleTarget(currentMode, modeCount);
 *   if (digitPressed)  want = digit;                           // absolute jump
 *   dxr::ModeSwitchFrame f = g_modeSwitch.step(dt_seconds, want, modeCount,
 *                                              modeViewCounts, currentMode,
 *                                              steadyIpd);
 *   rig.ipdFactor = f.ipd;                         // submit THIS value this frame
 *   if (f.fire)
 *       xrRequestDisplayRenderingModeDXR(session, f.mode);
 *   // currentMode follows XrEventDataRenderingModeChangedDXR, never f.mode.
 * @endcode
 *
 * The lower-level request()/update() pair is what step() is built from; use it
 * directly only when the app needs a different request policy.
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

//! What one ModeSwitch::step() call asks the app to do this frame.
struct ModeSwitchFrame
{
	float ipd = 1.0f;  //!< ipdFactor to submit on the view rig THIS frame
	bool fire = false; //!< true → call xrRequestDisplayRenderingModeDXR(session, mode) NOW
	uint32_t mode = 0; //!< the mode index to request when fire is true
};

//! Default ramp duration (seconds). Long enough to read as a transition, short
//! enough that a V press still feels immediate. Shared by every DisplayXR app.
constexpr float kModeSwitchDefaultDurationS = 0.18f;

//! Smooth 2D<->3D mode-switch sequencer. One instance per session.
class ModeSwitch
{
public:
	//! Ramp duration (seconds) and easing curve. duration_s <= 0 → instant
	//! (fire immediately, no ramp). Defaults (no configure() call needed):
	//! kModeSwitchDefaultDurationS, SmoothStep.
	void
	configure(float duration_s, ModeSwitchEasing easing = ModeSwitchEasing::SmoothStep)
	{
		dur_ = duration_s > 0.0f ? duration_s : 0.0f;
		easing_ = easing;
	}

	//! The mode a V-style "cycle" key should target: currentMode + 1, wrapping.
	//! Returns -1 when there are no modes.
	static int32_t
	cycleTarget(uint32_t currentMode, uint32_t modeCount)
	{
		return modeCount > 0 ? (int32_t)((currentMode + 1) % modeCount) : -1;
	}

	//! Per-frame glue — THE call every app makes, once per frame, on every
	//! platform. Funnels this frame's user request (if any) into the sequencer,
	//! advances the ramp, and reports what to submit / request.
	//!
	//!  dt_s           : wall-clock seconds since the previous call
	//!  requestedMode  : -1 = no request this frame; else the mode index the user
	//!                   asked for (cycleTarget() for V, the digit for 0-8).
	//!                   Out-of-range indices are ignored. Pass -1 while the app
	//!                   cannot request (no session / no PFN) — the ramp still
	//!                   advances.
	//!  modeCount      : number of rendering modes (xrEnumerateDisplayRenderingModesDXR)
	//!  modeViewCounts : per-mode viewCount from the same enumeration (may be
	//!                   null → every mode treated as 2D-dimensioned = 1 view)
	//!  currentMode    : the runtime's CURRENT mode (the enumerate's isActive,
	//!                   then XrEventDataRenderingModeChangedDXR)
	//!  steadyIpd      : the app's full-3D ipdFactor (the user-tuned value, NOT
	//!                   assumed 1.0); returned verbatim while idle
	//!
	//! fire is already suppressed when the target equals currentMode (a no-op
	//! reversal), so the caller requests unconditionally when fire is true.
	ModeSwitchFrame
	step(float dt_s,
	     int32_t requestedMode,
	     uint32_t modeCount,
	     const uint32_t *modeViewCounts,
	     uint32_t currentMode,
	     float steadyIpd)
	{
		auto vcOf = [&](uint32_t m) -> uint32_t {
			return (modeViewCounts != nullptr && m < modeCount && modeViewCounts[m] > 0) ? modeViewCounts[m]
			                                                                              : 1u;
		};

		if (requestedMode >= 0 && (uint32_t)requestedMode < modeCount) {
			// currentIpd = the disparity actually on screen right now: the last
			// ramp output while a ramp is in flight, else the steady value (what
			// the idle branch renders). NOT the bare ipd() — it is 0 until the
			// first ramp ever runs, so the FIRST 3D->2D would ramp 0->0 and fire
			// immediately = a one-time snap from full disparity.
			const float currentIpd = active() ? cur_ : steadyIpd;
			request((uint32_t)requestedMode, vcOf((uint32_t)requestedMode), currentMode, vcOf(currentMode),
			        currentIpd, steadyIpd);
		}

		ModeSwitchFrame f;
		f.ipd = steadyIpd; // idle: render the tuned steady value (tracks +/- live)
		f.mode = currentMode;
		if (active()) {
			update(dt_s, &f.ipd, &f.fire, &f.mode);
			if (f.fire && f.mode == currentMode) {
				f.fire = false; // reversal back to where the runtime already is
			}
		}
		return f;
	}

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
	void
	request(uint32_t targetMode,
	        uint32_t targetVC,
	        uint32_t currentMode,
	        uint32_t currentVC,
	        float currentIpd,
	        float steadyIpd)
	{
		const bool toMono = targetVC <= 1;
		const bool fromMono = currentVC <= 1;

		targetMode_ = targetMode;
		from_ = currentIpd;

		if (toMono && !fromMono) {
			// 3D -> 2D: flatten the disparity first, then switch on landing so
			// 2D engages on already-mono content. The mode request is held
			// until the ramp completes (fireAtEnd_).
			phase_ = Phase::RampDownThenFire;
			to_ = 0.0f;
			fireAtEnd_ = true;
			firePending_ = false;
		} else if (!toMono && fromMono) {
			// 2D -> 3D: switch now so the first 3D frame is flat (from_ = 0
			// forces it regardless of any stale ipd), then ease the disparity
			// up to steady. The runtime's #615 coherence guard absorbs the
			// one-frame DP eye-set lag on that first frame.
			phase_ = Phase::FireThenRampUp;
			from_ = 0.0f;
			to_ = steadyIpd;
			firePending_ = true;
			fireAtEnd_ = false;
		} else {
			// Same dimensionality: 2D->2D, 3D->3D, or a reversal of a
			// not-yet-fired ->2D (the runtime is still in 3D, so currentVC is
			// still >1). No flatten needed — switch now (skip the fire when
			// it's a no-op back to the current mode) and restore steady
			// disparity for 3D modes; for 2D the disparity is irrelevant
			// (mono), so leave it where it is.
			phase_ = Phase::FireThenRampUp;
			to_ = toMono ? currentIpd : steadyIpd;
			firePending_ = (targetMode != currentMode);
			fireAtEnd_ = false;
		}

		t_ = dur_ > 0.0f ? 0.0f : 1.0f;
		cur_ = from_;
	}

	//! Advance the ramp by dt_s and report this frame's outputs.
	//!  *outIpd       : the ipdFactor to submit on the rig this frame
	//!  *outFire      : true on exactly ONE frame — when the caller should issue
	//!                  xrRequestDisplayRenderingModeDXR(*outMode). The caller
	//!                  should still skip the call when *outMode == currentMode
	//!                  (a no-op reversal), though the runtime tolerates it.
	//!                  (step() does that skip for you.)
	//!  *outMode      : the mode index to request when *outFire is true
	//! Any of the out-pointers may be null.
	void
	update(float dt_s, float *outIpd, bool *outFire, uint32_t *outMode)
	{
		bool fire = false;

		if (phase_ != Phase::Idle) {
			// Advance the normalized ramp.
			if (t_ < 1.0f && dur_ > 0.0f) {
				t_ += dt_s / dur_;
				if (t_ > 1.0f)
					t_ = 1.0f;
			} else {
				t_ = 1.0f;
			}
			cur_ = from_ + (to_ - from_) * ease(t_);

			if (phase_ == Phase::FireThenRampUp) {
				if (firePending_) {
					firePending_ = false;
					fire = true;
				}
				if (t_ >= 1.0f)
					phase_ = Phase::Idle;
			} else { // RampDownThenFire
				if (t_ >= 1.0f) {
					if (fireAtEnd_) {
						fireAtEnd_ = false;
						fire = true;
					}
					phase_ = Phase::Idle;
				}
			}
		}

		if (outIpd != nullptr)
			*outIpd = cur_;
		if (outFire != nullptr)
			*outFire = fire;
		if (outMode != nullptr)
			*outMode = targetMode_;
	}

	//! True while a ramp is in flight or a mode request is still pending.
	bool
	active() const
	{
		return phase_ != Phase::Idle;
	}

	//! Current ipdFactor without advancing the clock.
	float
	ipd() const
	{
		return cur_;
	}

private:
	enum class Phase
	{
		Idle,
		RampDownThenFire, //!< 3D->2D: ramp disparity to 0, THEN fire the switch
		FireThenRampUp    //!< ->3D or same-dim: fire now, THEN ease disparity
	};

	float
	ease(float t) const
	{
		if (t <= 0.0f)
			return 0.0f;
		if (t >= 1.0f)
			return 1.0f;
		switch (easing_) {
		case ModeSwitchEasing::Linear: return t;
		case ModeSwitchEasing::EaseOutCubic: {
			float u = 1.0f - t;
			return 1.0f - u * u * u;
		}
		case ModeSwitchEasing::SmoothStep:
		default: return t * t * (3.0f - 2.0f * t);
		}
	}

	Phase phase_ = Phase::Idle;
	uint32_t targetMode_ = 0;
	bool firePending_ = false; //!< FireThenRampUp: emit fire on the next update
	bool fireAtEnd_ = false;   //!< RampDownThenFire: emit fire when the ramp lands

	float from_ = 0.0f;
	float to_ = 0.0f;
	float cur_ = 0.0f; //!< last evaluated ipd
	float t_ = 1.0f;   //!< normalized progress; 1 = landed/idle
	float dur_ = kModeSwitchDefaultDurationS;
	ModeSwitchEasing easing_ = ModeSwitchEasing::SmoothStep;
};

} // namespace dxr

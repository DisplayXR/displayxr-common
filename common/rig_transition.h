// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Stateful, interruptible rig-transition controller.
 *
 * A thin time + easing wrapper around the pure dxr_rig_transition() core
 * (displayxr-common's dxr_view_math). The core does the convert-then-lerp; this
 * owns the clock, the easing curve, and the interruptibility convention
 * (retarget = snapshot the current eval as the new `from`, restart t). Platform-
 * neutral (no <windows.h>, no graphics types) so every demo / app shares one
 * transition implementation instead of hand-rolling from/to/t bookkeeping.
 */
#pragma once

#include "dxr_view_math.h" // dxr_rig, dxr_rig_transition, dxr_rig_display_info

namespace dxr {

enum class Easing {
	Linear,
	SmoothStep,  //!< Hermite 3t^2-2t^3 (ease in/out)
	EaseOutCubic //!< 1-(1-t)^3
};

//! Bounded enter/exit rig transition. The helper owns ONLY the move between two
//! rig snapshots; the app computes the endpoints and runs any orbit/fly steady-
//! state once the transition is done().
class RigTransition
{
public:
	//! Begin a transition from `from` to `to` over duration_s seconds.
	//! comfort_far_z bounds stereo comfort for camera-type output (<= 0 = the
	//! conservative infinity clamp); the scene far plane (world distance from the
	//! eye) lets a shallow scene converge nearer. duration_s <= 0 → instant.
	void start(const dxr_rig &from,
	           const dxr_rig &to,
	           const dxr_rig_display_info &info,
	           float duration_s,
	           Easing easing = Easing::SmoothStep,
	           float comfort_far_z = 0.0f);

	//! Interrupt: snapshot the CURRENT eval as the new `from` and animate to
	//! newTo over the original duration/easing/comfort budget, restarting t at 0.
	//! Seamless because the new `from` is exactly where the scene is right now.
	void retarget(const dxr_rig &newTo);

	//! Advance by dt_s and write the current rig to *out. Returns true while the
	//! transition is still running (t < 1), false once it has landed on `to`.
	bool update(float dt_s, dxr_rig *out);

	//! Evaluate at the current t without advancing the clock.
	void eval(dxr_rig *out) const;

	//! True while a transition is in flight (t < 1).
	bool active() const { return t_ < 1.0f; }

private:
	float curve(float t) const;

	dxr_rig from_{};
	dxr_rig to_{};
	dxr_rig_display_info info_{};
	float t_ = 1.0f; //!< normalized progress; starts at 1 (idle/landed)
	float dur_ = 0.0f;
	float comfort_far_z_ = 0.0f;
	Easing easing_ = Easing::SmoothStep;
};

} // namespace dxr

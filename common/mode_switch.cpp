// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Implementation of the 2D<->3D mode-switch sequencer — see
 *         mode_switch.h.
 *
 * The whole helper is the dimensionality-transition decision in request() plus
 * a scalar disparity ramp in update(). Calling request() mid-flight fully
 * resets the phase/ramp from the caller-supplied current disparity, so it
 * doubles as the retarget: a pending (un-fired) ->2D that the user reverses is
 * dropped automatically when the new request overwrites the phase.
 */
#include "mode_switch.h"

namespace dxr {

void
ModeSwitch::configure(float duration_s, ModeSwitchEasing easing)
{
	dur_ = duration_s > 0.0f ? duration_s : 0.0f;
	easing_ = easing;
}

float
ModeSwitch::ease(float t) const
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

void
ModeSwitch::request(uint32_t targetMode,
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
		// 3D -> 2D: flatten the disparity first, then switch on landing so 2D
		// engages on already-mono content. The mode request is held until the
		// ramp completes (fireAtEnd_).
		phase_ = Phase::RampDownThenFire;
		to_ = 0.0f;
		fireAtEnd_ = true;
		firePending_ = false;
	} else if (!toMono && fromMono) {
		// 2D -> 3D: switch now so the first 3D frame is flat (from_ = 0 forces
		// it regardless of any stale ipd), then ease the disparity up to steady.
		// The runtime's #615 coherence guard absorbs the one-frame DP eye-set
		// lag on that first frame.
		phase_ = Phase::FireThenRampUp;
		from_ = 0.0f;
		to_ = steadyIpd;
		firePending_ = true;
		fireAtEnd_ = false;
	} else {
		// Same dimensionality: 2D->2D, 3D->3D, or a reversal of a not-yet-fired
		// ->2D (the runtime is still in 3D, so currentVC is still >1). No
		// flatten needed — switch now (skip the fire when it's a no-op back to
		// the current mode) and restore steady disparity for 3D modes; for 2D
		// the disparity is irrelevant (mono), so leave it where it is.
		phase_ = Phase::FireThenRampUp;
		to_ = toMono ? currentIpd : steadyIpd;
		firePending_ = (targetMode != currentMode);
		fireAtEnd_ = false;
	}

	t_ = dur_ > 0.0f ? 0.0f : 1.0f;
	cur_ = from_;
}

void
ModeSwitch::update(float dt_s, float *outIpd, bool *outFire, uint32_t *outMode)
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

} // namespace dxr

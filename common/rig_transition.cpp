// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Implementation of the stateful rig-transition controller — see
 *         rig_transition.h. The math lives in dxr_rig_transition().
 */
#include "rig_transition.h"

namespace dxr {

float
RigTransition::curve(float t) const
{
	if (t <= 0.0f)
		return 0.0f;
	if (t >= 1.0f)
		return 1.0f;
	switch (easing_) {
	case Easing::Linear: return t;
	case Easing::EaseOutCubic: {
		float u = 1.0f - t;
		return 1.0f - u * u * u;
	}
	case Easing::SmoothStep:
	default: return t * t * (3.0f - 2.0f * t);
	}
}

void
RigTransition::start(const dxr_rig &from,
                     const dxr_rig &to,
                     const dxr_rig_display_info &info,
                     float duration_s,
                     Easing easing,
                     float comfort_far_z)
{
	from_ = from;
	to_ = to;
	info_ = info;
	dur_ = duration_s;
	easing_ = easing;
	comfort_far_z_ = comfort_far_z;
	t_ = (duration_s > 0.0f) ? 0.0f : 1.0f;
}

void
RigTransition::retarget(const dxr_rig &newTo)
{
	dxr_rig now;
	eval(&now);    // current rendered rig, in the OLD target's type
	from_ = now;   // continue seamlessly from here
	to_ = newTo;
	t_ = (dur_ > 0.0f) ? 0.0f : 1.0f;
}

void
RigTransition::eval(dxr_rig *out) const
{
	dxr_rig_transition(&from_, &to_, &info_, curve(t_), comfort_far_z_, out);
}

bool
RigTransition::update(float dt_s, dxr_rig *out)
{
	if (t_ < 1.0f && dur_ > 0.0f) {
		t_ += dt_s / dur_;
		if (t_ > 1.0f)
			t_ = 1.0f;
	} else {
		t_ = 1.0f;
	}
	eval(out);
	return t_ < 1.0f;
}

} // namespace dxr

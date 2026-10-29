// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Gamepad sample -> InputState mapping (pure; no OpenXR calls).
 *
 * Kept apart from gamepad_actions.cpp so the mapping (deadzone, rising edges,
 * trigger latch) links and tests without an OpenXR loader.
 */

#include "gamepad_actions.h"

#include <cmath>

namespace dxr {

namespace {

//! Radial deadzone, rescaled so the output rises continuously from 0 at the
//! deadzone edge to 1 at full deflection.
void
ApplyStickDeadzone(float x, float y, float& outX, float& outY)
{
    const float mag = std::sqrt(x * x + y * y);
    if (mag <= kGamepadStickDeadzone) {
        outX = outY = 0.0f;
        return;
    }
    float scaled = (mag - kGamepadStickDeadzone) / (1.0f - kGamepadStickDeadzone);
    if (scaled > 1.0f) scaled = 1.0f;
    const float k = scaled / mag;
    outX = x * k;
    outY = y * k;
}

//! One trigger as a level control with a release latch. Returns true when
//! @p outValue must be written to the tuned factor this poll.
bool
TuneTrigger(GamepadTriggerLatch& latch, float value, double nowSec, float currentFactor, float& outValue)
{
    if (value > kGamepadTriggerDeadzone) {
        if (!latch.held) {
            latch.held = true;
            latch.settledValid = false;
            latch.before = currentFactor;
        } else {
            const double dt = nowSec - latch.lastTimeSec;
            if (dt > 0.0) {
                const double rate = std::fabs((double)value - (double)latch.lastValue) / dt;
                if (rate < (double)kGamepadTriggerSettleRate) {
                    latch.settled = value;
                    latch.settledValid = true;
                }
            }
        }
        latch.lastValue = value;
        latch.lastTimeSec = nowSec;
        outValue = value;
        return true;
    }
    if (latch.held) {
        // Released: keep the level the trigger was held at, not the values it
        // swept through on the way out. A tap that never settled restores the
        // factor it found.
        latch.held = false;
        outValue = latch.settledValid ? latch.settled : latch.before;
        return true;
    }
    return false;
}

bool
RisingEdge(bool now, bool& prev)
{
    const bool edge = now && !prev;
    prev = now;
    return edge;
}

} // namespace

void
GamepadActionsApply(GamepadActions& pad, const GamepadSample& s, double nowSec, InputState& state)
{
    ApplyStickDeadzone(s.moveX, s.moveY, state.padMoveX, state.padMoveY);
    ApplyStickDeadzone(s.lookX, s.lookY, state.padLookX, state.padLookY);
    state.padAscend = s.ascend ? 1.0f : 0.0f;
    state.padDescend = s.descend ? 1.0f : 0.0f;

    // One-shot requests, same flags the keyboard sets.
    if (RisingEdge(s.resetView, pad.prevResetView)) state.resetViewRequested = true;          // Space
    if (RisingEdge(s.toggleRig, pad.prevToggleRig)) state.rigModeToggleRequested = true;      // C
    if (RisingEdge(s.toggleParallax, pad.prevToggleParallax))
        state.parallaxEnabled = !state.parallaxEnabled;                                       // P
    if (RisingEdge(s.fullscreen, pad.prevFullscreen)) state.fullscreenToggleRequested = true; // F11

    // Trigger tuning. Right = IPD: edits steadyIpdFactor (the ModeSwitch ramp
    // target) with ipdFactor seeded in lockstep, as the +/- keys do. Left =
    // parallax: parallaxFactor, as Ctrl+wheel does. Both over [0,1]; the
    // camera rig's per-frame IPD ceiling in UpdateCameraMovement still applies.
    bool tuned = false;
    float v = 0.0f;
    if (TuneTrigger(pad.ipdLatch, s.triggerRight, nowSec, state.viewParams.steadyIpdFactor, v)) {
        if (v > 1.0f) v = 1.0f;
        if (v < 0.0f) v = 0.0f;
        state.viewParams.steadyIpdFactor = v;
        state.viewParams.ipdFactor = v;
        tuned = true;
    }
    if (TuneTrigger(pad.parallaxLatch, s.triggerLeft, nowSec, state.viewParams.parallaxFactor, v)) {
        if (v > 1.0f) v = 1.0f;
        if (v < 0.0f) v = 0.0f;
        state.viewParams.parallaxFactor = v;
        tuned = true;
    }

    const bool anyInput = tuned || state.padMoveX != 0.0f || state.padMoveY != 0.0f || state.padLookX != 0.0f ||
                          state.padLookY != 0.0f || s.ascend || s.descend || s.resetView || s.toggleRig ||
                          s.toggleParallax || s.fullscreen;
    if (anyInput) {
        // Same as a key press (input_handler.cpp MarkUserInput).
        state.lastInputTimeSec = nowSec;
        state.animationActive = false;
    }
}

} // namespace dxr

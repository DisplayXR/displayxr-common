// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Gamepad camera control through standard OpenXR actions on /user/gamepad.
 *
 * The runtime publishes a pad as /user/gamepad on the
 * /interaction_profiles/microsoft/xbox_controller profile. This helper owns
 * one action set ("dxr_gamepad", priority 0) bound to that profile and maps it
 * onto InputState, so a pad drives the app's own camera and tunables next to
 * the keyboard:
 *
 *   | pad input                          | action          | InputState effect                         | key   |
 *   |------------------------------------|-----------------|-------------------------------------------|-------|
 *   | thumbstick_left                    | move            | padMoveX / padMoveY (strafe / forward)    | WASD  |
 *   | thumbstick_right                   | look            | padLookX / padLookY (yaw / pitch, 90°/s)  | mouse |
 *   | shoulder_right/click               | ascend          | padAscend                                 | E     |
 *   | shoulder_left/click                | descend         | padDescend                                | Q     |
 *   | menu/click                         | reset_view      | resetViewRequested (rising edge)          | Space |
 *   | view/click                         | toggle_rig      | rigModeToggleRequested (rising edge)      | C     |
 *   | y/click                            | toggle_parallax | parallaxEnabled flipped (rising edge)     | P     |
 *   | x/click                            | fullscreen      | fullscreenToggleRequested (rising edge)   | F11   |
 *   | trigger_right/value                | tune_ipd        | viewParams.steadyIpdFactor + ipdFactor    | +/-   |
 *   | trigger_left/value                 | tune_parallax   | viewParams.parallaxFactor                 | Ctrl+wheel |
 *
 * Sticks get a radial 0.15 deadzone (rescaled, so output is continuous from
 * 0). Triggers are LEVELS over [0,1], not rates: while a trigger is past 0.05
 * its value is written to the factor live; on release the factor latches to
 * the last value the trigger was HELD at (sampled while it moved slower than
 * a release does), so letting go never snaps the factor back to 0. A quick
 * tap that never settles restores the factor it found.
 *
 * OpenXR allows xrAttachSessionActionSets ONCE per session, and an app may
 * already have action sets of its own. So this helper never attaches: the app
 * puts GamepadActionsSet() into its own attach call and
 * GamepadActionsAppendActiveSet() into its own xrSyncActions array, then calls
 * GamepadActionsPoll() after each sync.
 *
 * The runtime gates /user/gamepad on "last-used input wins": idle pads stop
 * holding the role, the actions go inactive, and every pad field reads 0 —
 * keyboard / mouse behaviour is then exactly what it is without this helper.
 *
 * OpenXR-only (no platform headers); compiles on every platform.
 *
 * Usage:
 * @code
 *   dxr::GamepadActions pad;
 *   dxr::GamepadActionsCreate(instance, pad);          // before the attach
 *   XrActionSet sets[] = {myHandSet, dxr::GamepadActionsSet(pad)};
 *   // ... xrAttachSessionActionSets(session, {sets, 2}) ...
 *   // per frame:
 *   XrActiveActionSet active[2] = {{myHandSet, XR_NULL_PATH}};
 *   uint32_t n = 1;
 *   if (dxr::GamepadActionsAppendActiveSet(pad, &active[n])) n++;
 *   // ... xrSyncActions(session, {active, n}) ...
 *   dxr::GamepadActionsPoll(session, pad, inputState);
 *   // on exit:
 *   dxr::GamepadActionsDestroy(pad);
 * @endcode
 */

#pragma once

#include <cstdint>

#include <openxr/openxr.h>

#include "input_state.h"

namespace dxr {

//! Radial stick deadzone (fraction of full deflection).
constexpr float kGamepadStickDeadzone = 0.15f;
//! A trigger below this value is "released" and does not tune.
constexpr float kGamepadTriggerDeadzone = 0.05f;
//! Trigger value change per second below which the trigger counts as held
//! still (a release sweeps much faster); the latch samples only then.
constexpr float kGamepadTriggerSettleRate = 2.0f;

//! One poll's worth of pad input, already read from the action states.
//! Inactive actions read as 0 / false.
struct GamepadSample {
    bool active = false;          //!< any gamepad action was active this poll
    float moveX = 0.0f, moveY = 0.0f;   //!< left stick, raw [-1,1]
    float lookX = 0.0f, lookY = 0.0f;   //!< right stick, raw [-1,1]
    float triggerLeft = 0.0f;     //!< [0,1] tunes parallax
    float triggerRight = 0.0f;    //!< [0,1] tunes IPD
    bool ascend = false;          //!< right shoulder
    bool descend = false;         //!< left shoulder
    bool resetView = false;       //!< menu
    bool toggleRig = false;       //!< view
    bool toggleParallax = false;  //!< Y
    bool fullscreen = false;      //!< X
};

//! Per-trigger latch state (see the file comment).
struct GamepadTriggerLatch {
    bool held = false;
    bool settledValid = false;
    float settled = 0.0f;     //!< last value sampled while held still
    float lastValue = 0.0f;
    double lastTimeSec = 0.0;
    float before = 0.0f;      //!< the factor when the press began
};

struct GamepadActions {
    XrActionSet actionSet = XR_NULL_HANDLE;
    XrPath gamepadPath = XR_NULL_PATH;  //!< /user/gamepad (the one subaction path)

    XrAction move = XR_NULL_HANDLE;           //!< VECTOR2F
    XrAction look = XR_NULL_HANDLE;           //!< VECTOR2F
    XrAction ascend = XR_NULL_HANDLE;         //!< BOOLEAN
    XrAction descend = XR_NULL_HANDLE;        //!< BOOLEAN
    XrAction resetView = XR_NULL_HANDLE;      //!< BOOLEAN
    XrAction toggleRig = XR_NULL_HANDLE;      //!< BOOLEAN
    XrAction toggleParallax = XR_NULL_HANDLE; //!< BOOLEAN
    XrAction fullscreen = XR_NULL_HANDLE;     //!< BOOLEAN
    XrAction tuneIpd = XR_NULL_HANDLE;        //!< FLOAT
    XrAction tuneParallax = XR_NULL_HANDLE;   //!< FLOAT

    // Previous button states, for the rising-edge one-shot requests.
    bool prevResetView = false;
    bool prevToggleRig = false;
    bool prevToggleParallax = false;
    bool prevFullscreen = false;

    GamepadTriggerLatch ipdLatch;
    GamepadTriggerLatch parallaxLatch;
};

//! Create the action set + actions and suggest the xbox_controller bindings.
//! Call before the session's xrAttachSessionActionSets. On failure everything
//! created so far is destroyed and @p pad is left empty (the app runs without
//! pad control).
bool GamepadActionsCreate(XrInstance instance, GamepadActions& pad);

//! The action set to include in the app's single xrAttachSessionActionSets
//! call. XR_NULL_HANDLE when not created.
inline XrActionSet GamepadActionsSet(const GamepadActions& pad) { return pad.actionSet; }

//! Writes this helper's entry for the app's xrSyncActions active-set array
//! into @p out. Returns false (and writes nothing) when not created.
bool GamepadActionsAppendActiveSet(const GamepadActions& pad, XrActiveActionSet* out);

//! Read the action states after the app's xrSyncActions. Safe to call when the
//! sync failed or the session is not focused: the actions then read inactive.
void GamepadActionsRead(XrSession session, const GamepadActions& pad, GamepadSample& out);

//! Map one sample onto @p state (pure, no OpenXR calls): deadzoned sticks into
//! the pad* fields, rising-edge requests, trigger tuning with release latch,
//! and the lastInputTimeSec activity stamp. @p nowSec is in the
//! InputStateNowSec() time base.
void GamepadActionsApply(GamepadActions& pad, const GamepadSample& sample, double nowSec, InputState& state);

//! GamepadActionsRead + GamepadActionsApply at InputStateNowSec(). Call once
//! per frame after the app's xrSyncActions.
void GamepadActionsPoll(XrSession session, GamepadActions& pad, InputState& state);

//! Destroy the action set (and with it its actions). Idempotent.
void GamepadActionsDestroy(GamepadActions& pad);

} // namespace dxr

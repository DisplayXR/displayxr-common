// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Gamepad action set: creation, bindings and state reads (OpenXR calls).
 *
 * The pure input -> InputState mapping lives in gamepad_actions_apply.cpp so
 * it can be tested without an OpenXR loader.
 */

#include "gamepad_actions.h"

#include <cstdio>
#include <cstring>

namespace dxr {

namespace {

bool
CreateAction(GamepadActions& pad, XrActionType type, const char* name, const char* localized, XrAction& out)
{
    XrActionCreateInfo ai = {XR_TYPE_ACTION_CREATE_INFO};
    ai.actionType = type;
    std::snprintf(ai.actionName, sizeof(ai.actionName), "%s", name);
    std::snprintf(ai.localizedActionName, sizeof(ai.localizedActionName), "%s", localized);
    ai.countSubactionPaths = 1;
    ai.subactionPaths = &pad.gamepadPath;
    return XR_SUCCEEDED(xrCreateAction(pad.actionSet, &ai, &out));
}

float
ReadFloat(XrSession session, const GamepadActions& pad, XrAction action, bool& anyActive)
{
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = action;
    gi.subactionPath = pad.gamepadPath;
    XrActionStateFloat st = {XR_TYPE_ACTION_STATE_FLOAT};
    if (XR_FAILED(xrGetActionStateFloat(session, &gi, &st)) || !st.isActive) {
        return 0.0f;
    }
    anyActive = true;
    return st.currentState;
}

bool
ReadBool(XrSession session, const GamepadActions& pad, XrAction action, bool& anyActive)
{
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = action;
    gi.subactionPath = pad.gamepadPath;
    XrActionStateBoolean st = {XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_FAILED(xrGetActionStateBoolean(session, &gi, &st)) || !st.isActive) {
        return false;
    }
    anyActive = true;
    return st.currentState == XR_TRUE;
}

void
ReadVec2(XrSession session, const GamepadActions& pad, XrAction action, bool& anyActive, float& x, float& y)
{
    x = y = 0.0f;
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = action;
    gi.subactionPath = pad.gamepadPath;
    XrActionStateVector2f st = {XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_FAILED(xrGetActionStateVector2f(session, &gi, &st)) || !st.isActive) {
        return;
    }
    anyActive = true;
    x = st.currentState.x;
    y = st.currentState.y;
}

} // namespace

bool
GamepadActionsCreate(XrInstance instance, GamepadActions& pad)
{
    pad = GamepadActions{};

    XrActionSetCreateInfo setInfo = {XR_TYPE_ACTION_SET_CREATE_INFO};
    std::snprintf(setInfo.actionSetName, sizeof(setInfo.actionSetName), "%s", "dxr_gamepad");
    std::snprintf(setInfo.localizedActionSetName, sizeof(setInfo.localizedActionSetName), "%s",
                  "DisplayXR Gamepad");
    setInfo.priority = 0;
    if (XR_FAILED(xrCreateActionSet(instance, &setInfo, &pad.actionSet))) {
        pad.actionSet = XR_NULL_HANDLE;
        return false;
    }

    bool ok = XR_SUCCEEDED(xrStringToPath(instance, "/user/gamepad", &pad.gamepadPath));
    ok = ok && CreateAction(pad, XR_ACTION_TYPE_VECTOR2F_INPUT, "move", "Move", pad.move);
    ok = ok && CreateAction(pad, XR_ACTION_TYPE_VECTOR2F_INPUT, "look", "Look", pad.look);
    ok = ok && CreateAction(pad, XR_ACTION_TYPE_BOOLEAN_INPUT, "ascend", "Ascend", pad.ascend);
    ok = ok && CreateAction(pad, XR_ACTION_TYPE_BOOLEAN_INPUT, "descend", "Descend", pad.descend);
    ok = ok && CreateAction(pad, XR_ACTION_TYPE_BOOLEAN_INPUT, "reset_view", "Reset View", pad.resetView);
    ok = ok && CreateAction(pad, XR_ACTION_TYPE_BOOLEAN_INPUT, "toggle_rig", "Toggle Camera Rig", pad.toggleRig);
    ok = ok && CreateAction(pad, XR_ACTION_TYPE_BOOLEAN_INPUT, "toggle_parallax", "Toggle Parallax",
                            pad.toggleParallax);
    ok = ok && CreateAction(pad, XR_ACTION_TYPE_BOOLEAN_INPUT, "fullscreen", "Toggle Fullscreen", pad.fullscreen);
    ok = ok && CreateAction(pad, XR_ACTION_TYPE_FLOAT_INPUT, "tune_ipd", "Tune IPD", pad.tuneIpd);
    ok = ok && CreateAction(pad, XR_ACTION_TYPE_FLOAT_INPUT, "tune_parallax", "Tune Parallax", pad.tuneParallax);

    if (ok) {
        struct Binding {
            XrAction action;
            const char* path;
        };
        const Binding table[] = {
            {pad.move, "/user/gamepad/input/thumbstick_left"},
            {pad.look, "/user/gamepad/input/thumbstick_right"},
            {pad.ascend, "/user/gamepad/input/shoulder_right/click"},
            {pad.descend, "/user/gamepad/input/shoulder_left/click"},
            {pad.resetView, "/user/gamepad/input/menu/click"},
            {pad.toggleRig, "/user/gamepad/input/view/click"},
            {pad.toggleParallax, "/user/gamepad/input/y/click"},
            {pad.fullscreen, "/user/gamepad/input/x/click"},
            {pad.tuneIpd, "/user/gamepad/input/trigger_right/value"},
            {pad.tuneParallax, "/user/gamepad/input/trigger_left/value"},
        };
        constexpr uint32_t kCount = sizeof(table) / sizeof(table[0]);
        XrActionSuggestedBinding bindings[kCount] = {};
        for (uint32_t i = 0; ok && i < kCount; i++) {
            bindings[i].action = table[i].action;
            ok = XR_SUCCEEDED(xrStringToPath(instance, table[i].path, &bindings[i].binding));
        }
        XrPath profile = XR_NULL_PATH;
        ok = ok && XR_SUCCEEDED(xrStringToPath(instance, "/interaction_profiles/microsoft/xbox_controller", &profile));
        if (ok) {
            XrInteractionProfileSuggestedBinding suggest = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            suggest.interactionProfile = profile;
            suggest.suggestedBindings = bindings;
            suggest.countSuggestedBindings = kCount;
            ok = XR_SUCCEEDED(xrSuggestInteractionProfileBindings(instance, &suggest));
        }
    }

    if (!ok) {
        GamepadActionsDestroy(pad);
        return false;
    }
    return true;
}

bool
GamepadActionsAppendActiveSet(const GamepadActions& pad, XrActiveActionSet* out)
{
    if (pad.actionSet == XR_NULL_HANDLE || out == nullptr) {
        return false;
    }
    out->actionSet = pad.actionSet;
    out->subactionPath = XR_NULL_PATH;
    return true;
}

void
GamepadActionsRead(XrSession session, const GamepadActions& pad, GamepadSample& out)
{
    out = GamepadSample{};
    if (pad.actionSet == XR_NULL_HANDLE || session == XR_NULL_HANDLE) {
        return;
    }
    bool any = false;
    ReadVec2(session, pad, pad.move, any, out.moveX, out.moveY);
    ReadVec2(session, pad, pad.look, any, out.lookX, out.lookY);
    out.ascend = ReadBool(session, pad, pad.ascend, any);
    out.descend = ReadBool(session, pad, pad.descend, any);
    out.resetView = ReadBool(session, pad, pad.resetView, any);
    out.toggleRig = ReadBool(session, pad, pad.toggleRig, any);
    out.toggleParallax = ReadBool(session, pad, pad.toggleParallax, any);
    out.fullscreen = ReadBool(session, pad, pad.fullscreen, any);
    out.triggerRight = ReadFloat(session, pad, pad.tuneIpd, any);
    out.triggerLeft = ReadFloat(session, pad, pad.tuneParallax, any);
    out.active = any;
}

void
GamepadActionsPoll(XrSession session, GamepadActions& pad, InputState& state)
{
    GamepadSample sample;
    GamepadActionsRead(session, pad, sample);
    GamepadActionsApply(pad, sample, InputStateNowSec(), state);
}

void
GamepadActionsDestroy(GamepadActions& pad)
{
    if (pad.actionSet != XR_NULL_HANDLE) {
        // Destroying an action set also destroys its actions.
        xrDestroyActionSet(pad.actionSet);
    }
    pad = GamepadActions{};
}

} // namespace dxr

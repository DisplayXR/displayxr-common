// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Contract test for dxr::GamepadActionsApply (gamepad_actions.h) and,
 *         on Windows, the pad -> camera integration in UpdateCameraMovement.
 *
 * Pure: drives GamepadActionsApply with synthetic samples, so no OpenXR
 * loader or runtime is needed (the OpenXR-calling half lives in
 * gamepad_actions.cpp and is not linked in).
 */

#include <cmath>
#include <cstdio>

#include "gamepad_actions.h"

#ifdef _WIN32
#include "input_handler.h"
#endif

static int g_failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                       \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

static bool Near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

static void TestSticks()
{
    dxr::GamepadActions pad;
    InputState st;
    dxr::GamepadSample s;
    s.active = true;

    s.moveX = 0.1f; // inside the deadzone
    s.lookY = -0.14f;
    dxr::GamepadActionsApply(pad, s, 1.0, st);
    CHECK(st.padMoveX == 0.0f && st.padMoveY == 0.0f);
    CHECK(st.padLookX == 0.0f && st.padLookY == 0.0f);
    CHECK(st.lastInputTimeSec == 0.0); // nothing past the deadzone: not user input

    s.moveX = 1.0f;
    s.lookY = 0.5f;
    dxr::GamepadActionsApply(pad, s, 2.0, st);
    CHECK(Near(st.padMoveX, 1.0f));
    CHECK(Near(st.padLookY, (0.5f - 0.15f) / 0.85f));
    CHECK(st.lastInputTimeSec == 2.0);

    // Shoulders -> ascend / descend levels.
    s = dxr::GamepadSample{};
    s.active = true;
    s.ascend = true;
    dxr::GamepadActionsApply(pad, s, 3.0, st);
    CHECK(st.padAscend == 1.0f && st.padDescend == 0.0f);
    CHECK(st.padMoveX == 0.0f && st.padLookY == 0.0f);

    // Inactive (pad idle / role released): everything reads 0.
    dxr::GamepadActionsApply(pad, dxr::GamepadSample{}, 4.0, st);
    CHECK(st.padAscend == 0.0f && st.padDescend == 0.0f);
    CHECK(st.lastInputTimeSec == 3.0);
}

static void TestEdges()
{
    dxr::GamepadActions pad;
    InputState st;
    dxr::GamepadSample s;
    s.active = true;

    s.resetView = true;
    s.toggleRig = true;
    s.fullscreen = true;
    s.toggleParallax = true;
    const bool parallax0 = st.parallaxEnabled;
    dxr::GamepadActionsApply(pad, s, 1.0, st);
    CHECK(st.resetViewRequested && st.rigModeToggleRequested && st.fullscreenToggleRequested);
    CHECK(st.parallaxEnabled == !parallax0);

    // Held: no repeat.
    st.resetViewRequested = st.rigModeToggleRequested = st.fullscreenToggleRequested = false;
    dxr::GamepadActionsApply(pad, s, 1.1, st);
    CHECK(!st.resetViewRequested && !st.rigModeToggleRequested && !st.fullscreenToggleRequested);
    CHECK(st.parallaxEnabled == !parallax0);

    // Release, press again: fires again.
    dxr::GamepadActionsApply(pad, dxr::GamepadSample{}, 1.2, st);
    dxr::GamepadActionsApply(pad, s, 1.3, st);
    CHECK(st.resetViewRequested && st.rigModeToggleRequested && st.fullscreenToggleRequested);
    CHECK(st.parallaxEnabled == parallax0);
}

static void TestTriggerLatch()
{
    dxr::GamepadActions pad;
    InputState st;
    st.viewParams.steadyIpdFactor = st.viewParams.ipdFactor = 1.0f;
    double t = 10.0;
    const double dt = 1.0 / 60.0;
    dxr::GamepadSample s;
    s.active = true;

    // Squeeze slowly to 0.6 (0.6/s), hold, then let go quickly.
    for (int i = 1; i <= 60; i++) {
        s.triggerRight = 0.01f * (float)i;
        t += dt;
        dxr::GamepadActionsApply(pad, s, t, st);
        if (s.triggerRight > 0.05f) {
            CHECK(Near(st.viewParams.steadyIpdFactor, s.triggerRight)); // live while held
            CHECK(Near(st.viewParams.ipdFactor, s.triggerRight));
        }
    }
    for (int i = 0; i < 10; i++) {
        t += dt;
        dxr::GamepadActionsApply(pad, s, t, st);
    }
    CHECK(st.lastInputTimeSec == t);
    const float release[] = {0.45f, 0.25f, 0.08f, 0.0f};
    for (float r : release) {
        s.triggerRight = r;
        t += dt;
        dxr::GamepadActionsApply(pad, s, t, st);
    }
    CHECK(Near(st.viewParams.steadyIpdFactor, 0.6f));
    CHECK(Near(st.viewParams.ipdFactor, 0.6f));
    CHECK(Near(st.viewParams.parallaxFactor, 1.0f)); // right trigger is IPD only

    // Released stays released: further idle polls don't touch the factor.
    st.viewParams.steadyIpdFactor = 0.7f;
    dxr::GamepadActionsApply(pad, dxr::GamepadSample{}, t + 1.0, st);
    CHECK(Near(st.viewParams.steadyIpdFactor, 0.7f));

    // A tap that never settles restores the factor it found.
    s = dxr::GamepadSample{};
    s.active = true;
    st.viewParams.parallaxFactor = 0.8f;
    s.triggerLeft = 0.5f;
    dxr::GamepadActionsApply(pad, s, t + 2.0, st);
    CHECK(Near(st.viewParams.parallaxFactor, 0.5f));
    s.triggerLeft = 0.0f;
    dxr::GamepadActionsApply(pad, s, t + 2.0 + dt, st);
    CHECK(Near(st.viewParams.parallaxFactor, 0.8f));
}

#ifdef _WIN32
// Pad -> camera through UpdateCameraMovement with every key up.
static void TestCameraIntegration()
{
    // Keyboard reference: W held for 1 s moves 0.1 along -Z (m2v = 1, scale 1).
    InputState key;
    key.keyW = true;
    UpdateCameraMovement(key, 1.0f);
    CHECK(Near(key.cameraPosZ, -0.1f));

    // Full left stick forward == W.
    InputState fwd;
    fwd.padMoveY = 1.0f;
    UpdateCameraMovement(fwd, 1.0f);
    CHECK(Near(fwd.cameraPosX, key.cameraPosX) && Near(fwd.cameraPosY, key.cameraPosY) &&
          Near(fwd.cameraPosZ, key.cameraPosZ));

    // Half strafe right, right shoulder up.
    InputState mv;
    mv.padMoveX = 0.5f;
    mv.padAscend = 1.0f;
    UpdateCameraMovement(mv, 1.0f);
    CHECK(Near(mv.cameraPosX, 0.05f) && Near(mv.cameraPosY, 0.1f) && Near(mv.cameraPosZ, 0.0f));

    // Right stick right for 0.5 s: turn right 45 deg (yaw decreases, as the mouse).
    InputState look;
    look.padLookX = 1.0f;
    UpdateCameraMovement(look, 0.5f);
    CHECK(Near(look.yaw, -0.78539816f));
    CHECK(look.cameraPosX == 0.0f && look.cameraPosZ == 0.0f);

    // Look up is clamped like the mouse drag.
    InputState up;
    up.padLookY = 1.0f;
    UpdateCameraMovement(up, 2.0f);
    CHECK(Near(up.pitch, 1.4f));

    // All pad fields zero: nothing moves.
    InputState idle;
    UpdateCameraMovement(idle, 1.0f);
    CHECK(idle.cameraPosX == 0.0f && idle.cameraPosY == 0.0f && idle.cameraPosZ == 0.0f);
    CHECK(idle.yaw == 0.0f && idle.pitch == 0.0f);
}
#endif

int main()
{
    TestSticks();
    TestEdges();
    TestTriggerLatch();
#ifdef _WIN32
    TestCameraIntegration();
#endif
    if (g_failures != 0) {
        std::fprintf(stderr, "gamepad_actions_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("gamepad_actions_test: OK\n");
    return 0;
}

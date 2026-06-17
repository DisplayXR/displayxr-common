// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Keyboard and mouse input state tracking implementation
 */

#include "input_handler.h"
#include "display3d_view.h"
#include "rig_mode.h"  // shared display<->camera rig toggle + reset (C / SPACE)
#include "dxr_view_math.h"  // dxr_rig_max_ipd_factor (camera-rig IPD comfort ceiling)
#include <DirectXMath.h>
#include <chrono>
#include <cmath>
#include <sstream>

using namespace DirectX;

// Monotonic wall-clock seconds — matches macOS NowSec() semantics.
static double NowSec() {
    using namespace std::chrono;
    return (double)duration_cast<microseconds>(
        high_resolution_clock::now().time_since_epoch()).count() * 1e-6;
}

static void MarkUserInput(InputState& state) {
    state.lastInputTimeSec = NowSec();
    state.animationActive = false;
}

// Helper to get key name from virtual key code
static std::string GetKeyName(WPARAM vk) {
    // Handle special keys
    switch (vk) {
    case VK_SPACE: return "Space";
    case VK_RETURN: return "Enter";
    case VK_ESCAPE: return "Escape";
    case VK_TAB: return "Tab";
    case VK_BACK: return "Backspace";
    case VK_SHIFT: return "Shift";
    case VK_CONTROL: return "Ctrl";
    case VK_MENU: return "Alt";
    case VK_LEFT: return "Left";
    case VK_RIGHT: return "Right";
    case VK_UP: return "Up";
    case VK_DOWN: return "Down";
    case VK_F1: return "F1";
    case VK_F2: return "F2";
    case VK_F3: return "F3";
    case VK_F4: return "F4";
    case VK_F5: return "F5";
    case VK_F11: return "F11";
    default: break;
    }

    // Letters and numbers
    if (vk >= 'A' && vk <= 'Z') {
        return std::string(1, (char)vk);
    }
    if (vk >= '0' && vk <= '9') {
        return std::string(1, (char)vk);
    }

    // Unknown key
    std::ostringstream oss;
    oss << "0x" << std::hex << vk;
    return oss.str();
}

bool UpdateInputState(InputState& state, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_MOUSEMOVE:
        state.mouseX = LOWORD(lParam);
        state.mouseY = HIWORD(lParam);

        // Update camera rotation if dragging
        if (state.dragging) {
            MarkUserInput(state);
            int dx = state.mouseX - state.dragStartX;
            int dy = state.mouseY - state.dragStartY;
            state.yaw -= dx * 0.005f;
            state.pitch -= dy * 0.005f;
            // Clamp pitch to avoid gimbal lock
            if (state.pitch > 1.4f) state.pitch = 1.4f;
            if (state.pitch < -1.4f) state.pitch = -1.4f;
            state.dragStartX = state.mouseX;
            state.dragStartY = state.mouseY;
        }
        return true;

    case WM_LBUTTONDBLCLK:
        MarkUserInput(state);
        state.teleportRequested = true;
        state.teleportMouseX = (float)LOWORD(lParam);
        state.teleportMouseY = (float)HIWORD(lParam);
        return true;

    case WM_LBUTTONDOWN:
        MarkUserInput(state);
        state.leftButton = true;
        state.dragging = true;
        state.dragStartX = state.mouseX;
        state.dragStartY = state.mouseY;
        // SetCapture moved to app WndProc — calling it here causes reentrant
        // deadlock in multi-threaded apps that protect UpdateInputState with a mutex
        return true;

    case WM_LBUTTONUP:
        state.leftButton = false;
        state.dragging = false;
        // ReleaseCapture moved to app WndProc — same reason as above
        return true;

    case WM_RBUTTONDOWN:
        state.rightButton = true;
        return true;

    case WM_RBUTTONUP:
        state.rightButton = false;
        return true;

    case WM_MBUTTONDOWN:
        state.middleButton = true;
        return true;

    case WM_MBUTTONUP:
        state.middleButton = false;
        return true;

    case WM_MOUSEWHEEL: {
        MarkUserInput(state);
        int zDelta = GET_WHEEL_DELTA_WPARAM(wParam);
        float factor = (zDelta > 0) ? 1.1f : (1.0f / 1.1f);
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool alt   = (GetKeyState(VK_MENU) & 0x8000) != 0;
        if (shift) {
            // ipd and parallax are conceptually a single "3D effect strength"
            // knob — driving them in lockstep matches the +/- key bindings
            // and the macOS demo's behaviour.
            float v = state.viewParams.ipdFactor * factor;
            if (v < 0.0f) v = 0.0f;
            // Camera mode: the comfortable ceiling is M = 1/f (display-equivalent
            // 1), which needs display info not available in this WndProc — it's
            // enforced per-frame in UpdateCameraMovement. Only the display rig is
            // hard-capped at 1 here.
            if (!state.cameraMode && v > 1.0f) v = 1.0f;
            state.viewParams.ipdFactor = v;
            state.viewParams.parallaxFactor = v;
        } else if (ctrl) {
            state.viewParams.parallaxFactor *= factor;
            if (state.viewParams.parallaxFactor < 0.0f) state.viewParams.parallaxFactor = 0.0f;
            if (state.viewParams.parallaxFactor > 1.0f) state.viewParams.parallaxFactor = 1.0f;
        } else if (alt) {
            if (state.cameraMode) {
                state.viewParams.invConvergenceDistance *= factor;
                if (state.viewParams.invConvergenceDistance < 0.1f) state.viewParams.invConvergenceDistance = 0.1f;
                if (state.viewParams.invConvergenceDistance > 10.0f) state.viewParams.invConvergenceDistance = 10.0f;
            } else {
                // Cap perspectiveFactor at 1.0 — values above that exaggerate
                // the Kooima eye separation past the display's natural geometry
                // and look wrong (Z-fighting / hyperstereo on the splat scene).
                state.viewParams.perspectiveFactor *= factor;
                if (state.viewParams.perspectiveFactor < 0.1f) state.viewParams.perspectiveFactor = 0.1f;
                if (state.viewParams.perspectiveFactor > 1.0f) state.viewParams.perspectiveFactor = 1.0f;
            }
        } else {
            if (state.cameraMode) {
                state.viewParams.zoomFactor *= factor;
                if (state.viewParams.zoomFactor < 0.1f) state.viewParams.zoomFactor = 0.1f;
                if (state.viewParams.zoomFactor > 10.0f) state.viewParams.zoomFactor = 10.0f;
            } else {
                state.viewParams.scaleFactor *= factor;
                if (state.viewParams.scaleFactor < 0.1f) state.viewParams.scaleFactor = 0.1f;
                if (state.viewParams.scaleFactor > 10.0f) state.viewParams.scaleFactor = 10.0f;
            }
        }
        return true;
    }

    case WM_KEYDOWN:
        MarkUserInput(state);
        state.lastKey = GetKeyName(wParam);
        // Ctrl+T: transparent background toggle. Intercept before the
        // plain-'T' eye-tracking handler in the switch below.
        if (wParam == 'T' && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            state.transparentBgToggleRequested = true;
            return true;
        }
        switch (wParam) {
        case 'W': state.keyW = true; break;
        case 'A': state.keyA = true; break;
        case 'S': state.keyS = true; break;
        case 'D': state.keyD = true; break;
        case 'E': state.keyE = true; break;
        case 'Q': state.keyQ = true; break;
        case 'M':
            state.animateToggleRequested = true;
            break;
        case 'N':   // next glTF animation clip
            state.cycleClipRequested = true;
            break;
        case 'K':   // play/pause the active clip
            state.playPauseRequested = true;
            break;
        case VK_OEM_MINUS: {
            float v = state.viewParams.ipdFactor - 0.1f;
            if (v < 0.1f) v = 0.1f;
            state.viewParams.ipdFactor = v;
            state.viewParams.parallaxFactor = v;
            break;
        }
        case VK_OEM_PLUS: {
            float v = state.viewParams.ipdFactor + 0.1f;
            // Camera mode: clamped to M per-frame in UpdateCameraMovement; only the
            // display rig is hard-capped at 1 here (see the shift+wheel note above).
            if (!state.cameraMode && v > 1.0f) v = 1.0f;
            state.viewParams.ipdFactor = v;
            state.viewParams.parallaxFactor = v;
            break;
        }
        case VK_SPACE:
            state.resetViewRequested = true;
            break;
        case 'P':
            state.keyP = true;
            state.parallaxEnabled = !state.parallaxEnabled;
            break;
        case VK_F11:
            state.keyF11 = true;
            state.fullscreenToggleRequested = true;
            break;
        case VK_TAB:
            // hudToggleRequiresShift (default true): require SHIFT to avoid
            // colliding with the workspace shell's bare TAB binding (cycle
            // focus across windows) — a non-modified TAB falls through to the
            // shell rather than toggle the HUD. Standalone demos set the
            // field false at InputState init to keep bare-TAB HUD toggling.
            if (!state.hudToggleRequiresShift ||
                (GetKeyState(VK_SHIFT) & 0x8000) != 0) {
                state.hudVisible = !state.hudVisible;
            }
            break;
        case 'V':
            // Request a cycle to the next mode. Main loop reads the runtime's
            // current mode index (xr.currentModeIndex) to compute the target
            // and calls xrRequestDisplayRenderingModeEXT.
            state.cycleRenderingModeRequested = true;
            break;
        case 'I':
            // Snapshot the multi-view atlas to a PNG. Render thread
            // consumes the flag after xrEndFrame.
            state.captureAtlasRequested = true;
            break;
        case 'B':
            // #228 smoke test: ask the runtime for a Tier 1 spatial file
            // picker. Main loop consumes the flag, calls
            // xrRequestFilePickerEXT, polls events for the completion.
            state.filePickerRequestRequested = true;
            break;
        case '0':
            state.absoluteRenderingModeRequested = 0; // 2D mode
            break;
        case '1':
            if (state.renderingModeCount > 1) state.absoluteRenderingModeRequested = 1;
            break;
        case '2':
            if (state.renderingModeCount > 2) state.absoluteRenderingModeRequested = 2;
            break;
        case '3':
            if (state.renderingModeCount > 3) state.absoluteRenderingModeRequested = 3;
            break;
        case '4':
            if (state.renderingModeCount > 4) state.absoluteRenderingModeRequested = 4;
            break;
        case '5':
            if (state.renderingModeCount > 5) state.absoluteRenderingModeRequested = 5;
            break;
        case '6':
            if (state.renderingModeCount > 6) state.absoluteRenderingModeRequested = 6;
            break;
        case '7':
            if (state.renderingModeCount > 7) state.absoluteRenderingModeRequested = 7;
            break;
        case '8':
            if (state.renderingModeCount > 8) state.absoluteRenderingModeRequested = 8;
            break;
        case 'T':
            state.eyeTrackingModeToggleRequested = true;
            break;
        case 'C':
            // Request a disturbance-free rig round-trip. The actual conversion
            // runs in UpdateCameraMovement, which has the canvas dimensions and
            // converts the CURRENT rig to its exact equivalent in the other
            // parameterization (no jump). Replaces the former flip-and-reset.
            state.rigModeToggleRequested = true;
            break;
        }
        return true;

    case WM_KEYUP:
        switch (wParam) {
        case 'W': state.keyW = false; break;
        case 'A': state.keyA = false; break;
        case 'S': state.keyS = false; break;
        case 'D': state.keyD = false; break;
        case 'E': state.keyE = false; break;
        case 'Q': state.keyQ = false; break;
        case 'P': state.keyP = false; break;
        case VK_F11: state.keyF11 = false; break;
        }
        return true;
    }

    return false;
}

void UpdateCameraMovement(InputState& state, float deltaTime, float displayHeightM) {
    // Handle view reset (spacebar) — absolute, drift-free: snap back to the
    // app's initial DISPLAY-centric state (pose at origin / identity, the
    // initial vHeight, every tunable at its default incl. cameraM2v=1). Dumb and
    // absolute by design (no conversion, no dependence on the current rig) so
    // there is no room for drift. (The former reset preserved cameraMode/vHeight
    // and left cameraM2v stale, which skewed the scene after a camera-mode C.)
    if (state.resetViewRequested) {
        state.resetViewRequested = false;
        float pos[3] = {state.cameraPosX, state.cameraPosY, state.cameraPosZ};
        dxr::RigResetToInitial(state.viewParams, state.cameraMode, pos, state.yaw, state.pitch,
                               state.initialVirtualDisplayHeight);
        state.cameraPosX = pos[0];
        state.cameraPosY = pos[1];
        state.cameraPosZ = pos[2];
        state.teleportAnimating = false;
        state.transitioning = false;
        state.animateEnabled = false;
        state.animationActive = false;
        state.lastInputTimeSec = NowSec();
        return;
    }

    // Disturbance-free rig round-trip toggle (C key) — delegate to the shared
    // converter so Windows and macOS apps behave identically. Pass the same
    // orientation quaternion the app submits to the runtime, plus the CANVAS
    // size the runtime renders into (NOT the full display).
    if (state.rigModeToggleRequested) {
        state.rigModeToggleRequested = false;
        XMFLOAT4 rq;
        XMStoreFloat4(&rq, XMQuaternionRotationRollPitchYaw(state.pitch, state.yaw, 0.0f));
        const float quat[4] = {rq.x, rq.y, rq.z, rq.w};
        float pos[3] = {state.cameraPosX, state.cameraPosY, state.cameraPosZ};
        dxr::RigToggleMode(state.viewParams, state.cameraMode, pos, quat, state.canvasWidthM,
                           state.canvasHeightM, state.nominalViewerZ, displayHeightM);
        state.cameraPosX = pos[0];
        state.cameraPosY = pos[1];
        state.cameraPosZ = pos[2];
        state.lastInputTimeSec = NowSec();
        return;
    }

    // Smooth display-pose transition (slerp) — gaussian-splat demo path.
    if (state.transitioning) {
        state.transitionT += deltaTime;
        float u = state.transitionT / state.transitionDuration;
        if (u >= 1.0f) u = 1.0f;
        float invU = 1.0f - u;
        float eased = 1.0f - invU * invU * invU;   // ease-out cubic
        XrPosef cur;
        display3d_pose_slerp(&state.transitionFrom, &state.transitionTo, eased, &cur);
        state.cameraPosX = cur.position.x;
        state.cameraPosY = cur.position.y;
        state.cameraPosZ = cur.position.z;
        // Decompose quaternion back into yaw/pitch (DirectXMath RollPitchYaw convention).
        XMVECTOR q = XMVectorSet(cur.orientation.x, cur.orientation.y, cur.orientation.z, cur.orientation.w);
        XMVECTOR fwd = XMVector3Rotate(XMVectorSet(0, 0, -1, 0), q);
        XMFLOAT3 f;
        XMStoreFloat3(&f, fwd);
        // Inverse of XMQuaternionRotationRollPitchYaw(p, y, 0): for small angles,
        // fwd = q * (0,0,-1) ≈ (-sin(y), sin(p), -cos(y)) (same signs as macOS path).
        state.yaw = atan2f(-f.x, -f.z);
        float fy = f.y;
        if (fy > 1.0f) fy = 1.0f;
        if (fy < -1.0f) fy = -1.0f;
        state.pitch = asinf(fy);
        if (u >= 1.0f) state.transitioning = false;
        return;
    }

    // Meters-to-virtual conversion (matches Kooima projection scaling)
    float m2v = 1.0f;
    if (state.viewParams.virtualDisplayHeight > 0.0f && displayHeightM > 0.0f)
        m2v = state.viewParams.virtualDisplayHeight / displayHeightM;

    const float moveSpeed = 0.1f * m2v / state.viewParams.scaleFactor; // Virtual units per second, scaled with zoom

    // Build orientation quaternion using the same function as LocateViews,
    // guaranteeing movement vectors match the view rotation exactly.
    XMVECTOR ori = XMQuaternionRotationRollPitchYaw(state.pitch, state.yaw, 0.0f);

    // Derive direction vectors by rotating basis vectors with the quaternion
    XMFLOAT3 fwd, rt, up;
    XMStoreFloat3(&fwd, XMVector3Rotate(XMVectorSet(0, 0, -1, 0), ori));  // forward = -Z
    XMStoreFloat3(&rt,  XMVector3Rotate(XMVectorSet(1, 0, 0, 0), ori));   // right = +X
    XMStoreFloat3(&up,  XMVector3Rotate(XMVectorSet(0, 1, 0, 0), ori));   // up = +Y

    // W/S: move along display forward (fly mode)
    if (state.keyW) {
        state.cameraPosX += fwd.x * moveSpeed * deltaTime;
        state.cameraPosY += fwd.y * moveSpeed * deltaTime;
        state.cameraPosZ += fwd.z * moveSpeed * deltaTime;
    }
    if (state.keyS) {
        state.cameraPosX -= fwd.x * moveSpeed * deltaTime;
        state.cameraPosY -= fwd.y * moveSpeed * deltaTime;
        state.cameraPosZ -= fwd.z * moveSpeed * deltaTime;
    }
    // A/D: strafe along display right
    if (state.keyA) {
        state.cameraPosX -= rt.x * moveSpeed * deltaTime;
        state.cameraPosY -= rt.y * moveSpeed * deltaTime;
        state.cameraPosZ -= rt.z * moveSpeed * deltaTime;
    }
    if (state.keyD) {
        state.cameraPosX += rt.x * moveSpeed * deltaTime;
        state.cameraPosY += rt.y * moveSpeed * deltaTime;
        state.cameraPosZ += rt.z * moveSpeed * deltaTime;
    }
    // E/Q: move along display up/down
    if (state.keyE) {
        state.cameraPosX += up.x * moveSpeed * deltaTime;
        state.cameraPosY += up.y * moveSpeed * deltaTime;
        state.cameraPosZ += up.z * moveSpeed * deltaTime;
    }
    if (state.keyQ) {
        state.cameraPosX -= up.x * moveSpeed * deltaTime;
        state.cameraPosY -= up.y * moveSpeed * deltaTime;
        state.cameraPosZ -= up.z * moveSpeed * deltaTime;
    }

    // Teleport animation: exponential ease-out (legacy path used by cube_* apps).
    if (state.teleportAnimating) {
        float t = 1.0f - expf(-10.0f * deltaTime); // ~90% in 0.23s
        state.cameraPosX += (state.teleportTargetX - state.cameraPosX) * t;
        state.cameraPosY += (state.teleportTargetY - state.cameraPosY) * t;
        state.cameraPosZ += (state.teleportTargetZ - state.cameraPosZ) * t;
        float dx = state.teleportTargetX - state.cameraPosX;
        float dy = state.teleportTargetY - state.cameraPosY;
        float dz = state.teleportTargetZ - state.cameraPosZ;
        if (dx*dx + dy*dy + dz*dz < 1e-8f)
            state.teleportAnimating = false;
    }

    // Auto-orbit: if enabled and user idle > 10s, slowly yaw the display.
    if (state.animateEnabled && state.lastInputTimeSec > 0.0) {
        double idleFor = NowSec() - state.lastInputTimeSec;
        state.animationActive = (idleFor > 10.0);
        if (state.animationActive) {
            float rate = 6.2831853f / 20.0f; // one revolution per 20 seconds
            state.yaw += rate * deltaTime;
        }
    } else {
        state.animationActive = false;
    }

    // Camera-rig IPD/parallax comfort ceiling. The display rig is bounded to [0,1],
    // but the camera rig's comfortable maximum is M = 1/f — the factor whose
    // DISPLAY-centric equivalent is exactly 1 (f = m2v*invd*N). M depends on the live
    // convergence / m2v / nominal distance, available only here, so the IPD knobs in
    // UpdateInputState don't cap camera-mode IPD at 1 — we enforce M per-frame. This
    // keeps IPD correct as convergence changes (M>1 far, <1 near, +inf at infinity →
    // no ceiling). Display mode is left at its own [0,1] clamp.
    if (state.cameraMode) {
        dxr_rig_display_info info = {displayHeightM, 1.0f, state.nominalViewerZ};
        dxr_camera_rig cam = {};
        cam.m2v = state.viewParams.cameraM2v;
        cam.inv_convergence_distance = state.viewParams.invConvergenceDistance;
        float ipdMax = dxr_rig_max_ipd_factor(&cam, &info);
        if (state.viewParams.ipdFactor > ipdMax) state.viewParams.ipdFactor = ipdMax;
        if (state.viewParams.parallaxFactor > ipdMax) state.viewParams.parallaxFactor = ipdMax;
    }
}

std::string GetMouseButtonString(const InputState& state) {
    std::string result;
    if (state.leftButton) result += "[LMB]";
    if (state.rightButton) result += "[RMB]";
    if (state.middleButton) result += "[MMB]";
    if (result.empty()) result = "None";
    return result;
}

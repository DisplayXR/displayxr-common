// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  RMB window-drag for borderless transparent overlays (Win32).
 *
 * Shaped punch-through windows (dxr::ClickThroughRegion) have no title bar,
 * so moving them needs an in-content gesture. The convention (matching the
 * Unity desktop-avatar sample): RIGHT-button drag moves the window. Gating
 * to "the opaque element, not the transparent part" comes free — a shaped
 * window never receives mouse messages outside its region.
 *
 * Usage (top of WndProc, BEFORE the app's input handler sees the message so
 * a window-drag doesn't double as camera input):
 * @code
 *   static dxr::RmbWindowDrag s_drag;
 *   if (s_drag.handleMessage(hwnd, msg, wParam, lParam, g_borderless.load()))
 *       return 0;
 * @endcode
 */

#pragma once

#if defined(_WIN32)

#include <windows.h>

namespace dxr {

class RmbWindowDrag {
public:
    //! Feed every window message. @p active gates drag START (typically
    //! "borderless/transparent mode"); an in-flight drag always completes.
    //! Returns true when the message was consumed (caller should return 0
    //! and skip its own input handling for it).
    bool
    handleMessage(HWND hwnd, UINT msg, WPARAM /*wParam*/, LPARAM /*lParam*/, bool active)
    {
        switch (msg) {
        case WM_RBUTTONDOWN:
            if (!active) return false;
            dragging_ = true;
            SetCapture(hwnd);
            GetCursorPos(&last_);
            // Look exactly like an OS title-bar move to anything hooked on
            // the window: the windowed-weaving phase-snap (runtime#757 /
            // vendor DP) keys on the modal move loop's bracketing messages.
            SendMessage(hwnd, WM_ENTERSIZEMOVE, 0, 0);
            return true;
        case WM_MOUSEMOVE: {
            if (!dragging_) return false;
            POINT p;
            GetCursorPos(&p);
            if (p.x != last_.x || p.y != last_.y) {
                RECT wr;
                GetWindowRect(hwnd, &wr);
                SetWindowPos(hwnd, nullptr, wr.left + (p.x - last_.x), wr.top + (p.y - last_.y), 0,
                             0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                last_ = p;
            }
            return true;
        }
        case WM_RBUTTONUP:
            if (!dragging_) return false;
            dragging_ = false;
            ReleaseCapture();
            SendMessage(hwnd, WM_EXITSIZEMOVE, 0, 0); // phase re-snap point
            return true;
        case WM_CAPTURECHANGED:
            if (dragging_) {
                dragging_ = false;
                SendMessage(hwnd, WM_EXITSIZEMOVE, 0, 0);
            }
            return false; // observe only — let the app see capture loss too
        }
        return false;
    }

    bool dragging() const { return dragging_; }

private:
    bool dragging_ = false;
    POINT last_ = {};
};

} // namespace dxr

#endif // _WIN32

// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief Win32 window adapter for dxr::CursorDepth (cursor_depth.h).
 *
 * Pointer: GetCursorPos -> ScreenToClient over GetClientRect. Both are in the
 * process's DPI space, so the ratio is right whatever the awareness.
 *
 * OS cursor: Windows re-sets the cursor on every WM_SETCURSOR, so hiding it
 * means answering WM_SETCURSOR (HTCLIENT) with SetCursor(NULL). The adapter
 * subclasses the window procedure (SetWindowLongPtrW, chained with
 * CallWindowProcW — callable from the render thread, unlike
 * SetWindowSubclass) and, on each hide/show transition, posts itself a
 * private message so the change takes effect on the WINDOW's thread without
 * waiting for the next mouse move. Only the client area is affected: the
 * frame, title bar and other windows keep their cursors. The original
 * procedure is restored on destruction when ours is still installed.
 */
#ifdef _WIN32

#include "cursor_depth.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>

namespace dxr {
namespace {

const wchar_t *const kProp = L"DisplayXR.CursorDepth";

UINT
refreshMessage()
{
    static const UINT msg = RegisterWindowMessageW(L"DisplayXR.CursorDepth.Refresh");
    return msg;
}

struct Win32State
{
    HWND hwnd = nullptr;
    WNDPROC prev = nullptr;
    std::atomic<bool> hidden{false};
};

bool
pointerOverClient(HWND hwnd)
{
    POINT pt;
    if (!GetCursorPos(&pt) || WindowFromPoint(pt) != hwnd) return false;
    RECT rc;
    if (!GetClientRect(hwnd, &rc) || !ScreenToClient(hwnd, &pt)) return false;
    return pt.x >= rc.left && pt.x < rc.right && pt.y >= rc.top && pt.y < rc.bottom;
}

LRESULT CALLBACK
cursorWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Win32State *s = static_cast<Win32State *>(GetPropW(hwnd, kProp));
    if (s == nullptr) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT && s->hidden.load()) {
        SetCursor(nullptr);
        return TRUE;
    }
    if (msg == refreshMessage()) {
        if (pointerOverClient(hwnd)) {
            // Re-run the normal WM_SETCURSOR path: hidden -> NULL above; shown
            // -> the app's own handler (or DefWindowProc's class cursor).
            SendMessageW(hwnd, WM_SETCURSOR, (WPARAM)hwnd, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
        }
        return 0;
    }
    return CallWindowProcW(s->prev, hwnd, msg, wp, lp);
}

class Win32CursorPlatform final : public CursorPlatform
{
public:
    explicit Win32CursorPlatform(HWND hwnd) : m_state(new Win32State)
    {
        m_state->hwnd = hwnd;
        if (hwnd == nullptr || !IsWindow(hwnd)) return;
        refreshMessage();
        SetPropW(hwnd, kProp, m_state);
        m_state->prev = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&cursorWndProc)));
        if (m_state->prev == nullptr) RemovePropW(hwnd, kProp);
    }

    ~Win32CursorPlatform() override
    {
        HWND hwnd = m_state->hwnd;
        m_state->hidden = false;
        if (m_state->prev == nullptr || !IsWindow(hwnd)) {
            delete m_state;
            return;
        }
        if (reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC)) == &cursorWndProc) {
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(m_state->prev));
            RemovePropW(hwnd, kProp);
            delete m_state;
            PostMessageW(hwnd, WM_MOUSEMOVE, 0, 0); // let the app's cursor come back promptly
            return;
        }
        // Someone subclassed after us: leave our proc chained, inert (hidden =
        // false), and its state alive for the window's lifetime, rather than
        // cut their chain off. A few bytes, once per such window.
    }

    bool pointerUV(XrVector2f &uv) override
    {
        HWND hwnd = m_state->hwnd;
        if (hwnd == nullptr) return false;
        POINT pt;
        RECT rc;
        if (!GetCursorPos(&pt) || !ScreenToClient(hwnd, &pt) || !GetClientRect(hwnd, &rc)) return false;
        return cursorCanvasUV(pt.x, pt.y, rc.right - rc.left, rc.bottom - rc.top, false, uv);
    }

    void setOsCursorHidden(bool hidden) override
    {
        m_state->hidden = hidden;
        if (m_state->prev != nullptr) PostMessageW(m_state->hwnd, refreshMessage(), 0, 0);
    }

private:
    Win32State *m_state; // heap: may outlive us (see the destructor)
};

} // namespace

std::unique_ptr<CursorPlatform>
cursorPlatformWin32(void *hwnd)
{
    return std::make_unique<Win32CursorPlatform>(static_cast<HWND>(hwnd));
}

} // namespace dxr

#endif // _WIN32

// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief X11 (Xlib) window adapter for dxr::CursorDepth (cursor_depth.h).
 *
 * Header-only on purpose: displayxr::common does not link libX11, the app
 * that owns an Xlib window does. Include it in that app:
 *
 *     #include "cursor_depth_x11.h"
 *     cursor.setPlatform(dxr::cursorPlatformX11(display, window));
 *
 * Pointer: XQueryPointer relative to the window over XGetWindowAttributes'
 * size (two round trips per frame). OS cursor: an invisible 1x1 pixmap
 * cursor is defined on the window while hidden; showing restores the cursor
 * the window had (`restoreCursor`, default None = inherit the parent's).
 * Core Xlib only, no XFixes. If you call chain() from a thread other than
 * the one running your X event loop, call XInitThreads() first (Xlib rule).
 *
 * Apps on displayxr::linux_window (X11 or Wayland) should use its own
 * set_cursor() through the manual path instead — see cursor_depth.h.
 */
#pragma once

#include "cursor_depth.h"

#include <X11/Xlib.h>

namespace dxr {

class X11CursorPlatform final : public CursorPlatform
{
public:
    X11CursorPlatform(Display *dpy, Window win, Cursor restoreCursor = None)
        : m_dpy(dpy), m_win(win), m_restore(restoreCursor)
    {}

    ~X11CursorPlatform() override
    {
        if (m_dpy == nullptr) return;
        if (m_hidden) setOsCursorHidden(false);
        if (m_blank != None) XFreeCursor(m_dpy, m_blank);
        XFlush(m_dpy);
    }

    bool pointerUV(XrVector2f &uv) override
    {
        if (m_dpy == nullptr || m_win == 0) return false;
        Window root, child;
        int rx, ry, wx, wy;
        unsigned int mask;
        if (!XQueryPointer(m_dpy, m_win, &root, &child, &rx, &ry, &wx, &wy, &mask)) return false;
        XWindowAttributes a;
        if (!XGetWindowAttributes(m_dpy, m_win, &a)) return false;
        return cursorCanvasUV(wx, wy, a.width, a.height, false, uv);
    }

    void setOsCursorHidden(bool hidden) override
    {
        if (m_dpy == nullptr || m_win == 0 || hidden == m_hidden) return;
        m_hidden = hidden;
        if (hidden) {
            if (m_blank == None) {
                static const char zero[1] = {0};
                Pixmap pm = XCreateBitmapFromData(m_dpy, m_win, zero, 1, 1);
                XColor black = {};
                m_blank = XCreatePixmapCursor(m_dpy, pm, pm, &black, &black, 0, 0);
                XFreePixmap(m_dpy, pm);
            }
            XDefineCursor(m_dpy, m_win, m_blank);
        } else if (m_restore != None) {
            XDefineCursor(m_dpy, m_win, m_restore);
        } else {
            XUndefineCursor(m_dpy, m_win);
        }
        XFlush(m_dpy);
    }

private:
    Display *m_dpy;
    Window m_win;
    Cursor m_restore;
    Cursor m_blank = None;
    bool m_hidden = false;
};

inline std::unique_ptr<CursorPlatform>
cursorPlatformX11(Display *dpy, Window win, Cursor restoreCursor = None)
{
    return std::make_unique<X11CursorPlatform>(dpy, win, restoreCursor);
}

} // namespace dxr

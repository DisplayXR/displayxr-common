// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Compile + link proof for the header-only X11 adapter (cursor_depth_x11.h).
 *
 * Runs without a display: with no X server it only checks that an adapter on a
 * null Display is inert. With $DISPLAY set (e.g. under xvfb-run) it also maps a
 * window and exercises pointer UV + hide/show once.
 */
#include "cursor_depth_x11.h"

#include <cstdio>

int
main()
{
    { // No display: inert, never crashes.
        dxr::X11CursorPlatform none(nullptr, 0);
        XrVector2f uv{};
        if (none.pointerUV(uv)) return 1;
        none.setOsCursorHidden(true);
        none.setOsCursorHidden(false);
    }
    Display *dpy = XOpenDisplay(nullptr);
    if (dpy == nullptr) {
        std::printf("cursor_depth_x11_compile: no X display, inert checks only\n");
        return 0;
    }
    Window win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, 320, 200, 0, 0, 0);
    XMapWindow(dpy, win);
    XSync(dpy, False);
    {
        dxr::CursorDepth cursor;
        cursor.enable(5);
        cursor.setPlatform(dxr::cursorPlatformX11(dpy, win));
        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
        XrViewState vs{XR_TYPE_VIEW_STATE};
        cursor.chain(li, vs); // reads the pointer through XQueryPointer
        auto platform = dxr::cursorPlatformX11(dpy, win);
        platform->setOsCursorHidden(true);
        platform->setOsCursorHidden(false);
    }
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    std::printf("cursor_depth_x11_compile: ok\n");
    return 0;
}

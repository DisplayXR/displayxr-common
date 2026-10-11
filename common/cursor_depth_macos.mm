// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief Cocoa window adapter for dxr::CursorDepth (cursor_depth.h).
 *
 * Pointer: the window's mouseLocationOutsideOfEventStream converted into the
 * canvas view, over its bounds (flipped or not). Polled, so it needs no event
 * hook; call chain() from the thread that runs the AppKit loop, as every
 * DisplayXR macOS app does (off it, the read falls back to +[NSEvent
 * mouseLocation] converted through the window).
 *
 * OS cursor: +[NSCursor hide] / +[NSCursor unhide] (balanced: the helper
 * calls this on transitions only), marshalled to the main thread when called
 * from another. The hide is app-wide, but the helper only hides while the
 * pointer is over the canvas (no hint is chained otherwise, so willDraw is
 * false), exactly like the hand-written reference adoption.
 */
#import <AppKit/AppKit.h>

#include "cursor_depth.h"

namespace dxr {
namespace {

class CocoaCursorPlatform final : public CursorPlatform
{
public:
    explicit CocoaCursorPlatform(NSView *view) : m_view(view) {}

    ~CocoaCursorPlatform() override
    {
        if (m_hidden) apply(false);
    }

    bool pointerUV(XrVector2f &uv) override
    {
        NSView *view = m_view;
        NSWindow *window = view.window;
        if (view == nil || window == nil) return false;
        NSPoint pw;
        if ([NSThread isMainThread]) {
            pw = [window mouseLocationOutsideOfEventStream];
        } else {
            const NSPoint ps = [NSEvent mouseLocation];
            pw = [window convertRectFromScreen:NSMakeRect(ps.x, ps.y, 0, 0)].origin;
        }
        const NSPoint pv = [view convertPoint:pw fromView:nil];
        const NSRect b = view.bounds;
        return cursorCanvasUV(pv.x - b.origin.x, pv.y - b.origin.y, b.size.width, b.size.height,
                              ![view isFlipped], uv);
    }

    void setOsCursorHidden(bool hidden) override
    {
        if (hidden == m_hidden) return;
        m_hidden = hidden;
        apply(hidden);
    }

private:
    static void apply(bool hidden)
    {
        if ([NSThread isMainThread]) {
            if (hidden) [NSCursor hide]; else [NSCursor unhide];
        } else {
            dispatch_async(dispatch_get_main_queue(), ^{
              if (hidden) [NSCursor hide]; else [NSCursor unhide];
            });
        }
    }

    NSView *m_view; // the app keeps the view alive while the helper uses it
    bool m_hidden = false;
};

} // namespace

std::unique_ptr<CursorPlatform>
cursorPlatformCocoa(void *nsView)
{
    return std::make_unique<CocoaCursorPlatform>((__bridge NSView *)nsView);
}

} // namespace dxr

// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  X11 glue for the shared client-side header bar. See
 *         dxr_x11_chrome.h.
 */

#include "dxr_x11_chrome.h"

#include <X11/Xutil.h> // XDestroyImage

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace dxr_x11_chrome {

namespace {
GC g_gc = nullptr;
Window g_gcWin = 0;
std::vector<uint32_t> g_ximage; //!< the raster packed for the window visual
} // namespace

float
DesktopScale(Display *dpy)
{
	if (const char *e = getenv("DXR_CSD_SCALE")) {
		const float s = (float)atof(e);
		if (s >= 0.5f && s <= 4.0f) {
			return s;
		}
	}
	// X11 coordinates on this path are device pixels (XWayland with native
	// scaling, or plain Xorg), so the bar must be scaled like every other
	// window's decorations.
	if (dpy != nullptr) {
		if (const char *dpi = XGetDefault(dpy, "Xft", "dpi")) {
			const float d = (float)atof(dpi);
			if (d >= 48.f && d <= 480.f) {
				return d / 96.0f;
			}
		}
	}
	return 1.0f;
}

XRectangle
BarRect(const dxr_csd::TitleBar &bar, uint32_t winW)
{
	XRectangle r;
	r.x = 0;
	r.y = 0;
	r.width = (unsigned short)winW;
	r.height = (unsigned short)bar.height();
	return r;
}

void
Paint(dxr_csd::TitleBar &bar, Display *dpy, Window win, const Visual *visual, uint32_t winW)
{
	if (dpy == nullptr || win == 0 || visual == nullptr || winW == 0) {
		return;
	}
	XWindowAttributes wa = {};
	if (XGetWindowAttributes(dpy, win, &wa) == 0) {
		return;
	}
	bar.render(winW); // no-op unless dirty or resized
	const uint32_t h = bar.height();

	// Pack into the window visual's own pixel layout. On the depth-32 ARGB
	// visual the non-RGB bits ARE the (premultiplied) alpha, which is what
	// makes the translucent material and the rounded corners real; a depth-24
	// visual has no alpha, and the painter was told so (opaque, square bar).
	const uint32_t rgb = (uint32_t)(visual->red_mask | visual->green_mask | visual->blue_mask);
	const uint32_t alpha = wa.depth == 32 ? ~rgb : 0u;
	g_ximage.resize((size_t)winW * h);
	bar.pack(g_ximage.data(), winW, (uint32_t)visual->red_mask, (uint32_t)visual->green_mask,
	         (uint32_t)visual->blue_mask, alpha);

	// ZPixmap at 32 bpp is what every TrueColor visual of depth 24/32 uses.
	XImage *img = XCreateImage(dpy, const_cast<Visual *>(visual), (unsigned)wa.depth, ZPixmap, 0,
	                           reinterpret_cast<char *>(g_ximage.data()), winW, h, 32, (int)winW * 4);
	if (img == nullptr) {
		return;
	}
	img->byte_order = LSBFirst; // our uint32_t array is host (little-endian) order
	if (g_gc == nullptr || g_gcWin != win) {
		if (g_gc != nullptr) {
			XFreeGC(dpy, g_gc);
		}
		g_gc = XCreateGC(dpy, win, 0, nullptr);
		g_gcWin = win;
	}
	XPutImage(dpy, win, g_gc, img, 0, 0, 0, 0, winW, h);
	img->data = nullptr; // owned by g_ximage, not by Xlib
	XDestroyImage(img);
	XFlush(dpy);
}

void
Release(Display *dpy)
{
	if (dpy != nullptr && g_gc != nullptr) {
		XFreeGC(dpy, g_gc);
	}
	g_gc = nullptr;
	g_gcWin = 0;
}

} // namespace dxr_x11_chrome

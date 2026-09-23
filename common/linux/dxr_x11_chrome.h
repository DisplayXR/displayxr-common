// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  X11 glue for the shared client-side header bar.
 *
 * The bar itself — metrics, hit testing, interaction state, the translucent
 * rounded raster — is displayxr::csd (common/csd_titlebar.h,
 * displayxr-common#52), the ONE chrome implementation, shared with the
 * native-Wayland leg (dxr_wl_chrome.h). What lives here is only what is
 * X11-specific: the desktop scale (Xft.dpi), the bar's rect for the XShape
 * input region, and XPutImage into the top-level packed for its visual.
 * Used by DxrLinuxWindow (desc.x11_header_bar); lifted out of
 * displayxr-demo-modelviewer's linux/csd_titlebar_x11 so every demo shares it.
 *
 * WHY CLIENT-SIDE (unchanged): X11 gives a client no hook into a WM-owned move
 * (Windows has WM_WINDOWPOSCHANGING; X11 only reports the result via
 * ConfigureNotify), so the drag must be the app's for every step to go through
 * the display processor's lattice snap. A visible, ordinary title bar is what
 * drives that drag. Do NOT "simplify" this back to WM decorations;
 * DXR_X11_WM_DECORATIONS=1 is the deliberate, unsnapped escape hatch.
 *
 * GEOMETRY (unchanged): the top-level is bar + content; the CONTENT child is
 * the window bound to the runtime, so the bar never enters the canvas, the
 * Kooima projection, the swapchain or the atlas, and the weave never covers it.
 *
 * TRANSLUCENT + ROUNDED on the 32-bit ARGB visual (under GNOME's always-on
 * compositing manager): the premultiplied raster's alpha is real, so the
 * desktop shows through the bar and outside its rounded top corners. On the
 * opaque root visual the glue asks the painter for an opaque, square bar
 * instead (setSurfaceHasAlpha(false)). The content child stays opaque either
 * way and weaves exactly as before.
 */

#pragma once

#include "csd_titlebar.h" // displayxr::csd

#include <X11/Xlib.h>

#include <cstdint>

namespace dxr_x11_chrome {

//! The desktop scale for the bar: DXR_CSD_SCALE, else Xft.dpi / 96 (GNOME
//! publishes its scale to X11 clients that way: 192 at 200 %), else 1.
float
DesktopScale(Display *dpy);

//! The bar's rect in TOP-LEVEL px, for the click-through input region.
XRectangle
BarRect(const dxr_csd::TitleBar &bar, uint32_t winW);

//! Rasterise if dirty and XPutImage the bar into the top of @p win (the
//! TOP-LEVEL), @p winW wide. @p visual must be @p win's visual; the pixels are
//! packed through its channel masks (alpha = the non-RGB bits on a depth-32
//! visual).
void
Paint(dxr_csd::TitleBar &bar, Display *dpy, Window win, const Visual *visual, uint32_t winW);

//! Free the GC. The windows themselves belong to the caller.
void
Release(Display *dpy);

} // namespace dxr_x11_chrome

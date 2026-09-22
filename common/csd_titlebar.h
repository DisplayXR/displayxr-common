// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Client-side window chrome (a GNOME-style header bar) for weaving
 *         desktop windows — the ONE implementation, shared by every window
 *         system that needs it (displayxr-common#52).
 *
 * WHY CLIENT-SIDE. A windowed 3D app keeps the woven interlace phase right
 * only if the window's position is known to — and on X11, chosen by — the
 * app:
 *
 *   - X11: a WM-drawn title bar runs the drag inside the window manager's grab
 *     loop, so the client can only correct after the fact. An app-drawn bar
 *     makes the drag the app's own, and every step goes through the display
 *     processor's lattice snap (xrWeaveSnapWindowRectDXR).
 *   - Wayland: GNOME's mutter offers NO server-side decorations at all, so a
 *     native Wayland toplevel without client-side decorations has no title
 *     bar, no close / minimise, and can only be moved with Super+drag.
 *
 * WHY THIS FILE KNOWS NO WINDOW SYSTEM. What is shared is the part that must
 * look and behave the same everywhere — metrics, hit testing, interaction
 * state, and the raster itself. Getting the pixels on screen is the glue's job
 * and stays with the window system:
 *
 *   X11     : XPutImage into the top-level, packed for its visual (pack()).
 *   Wayland : a wl_subsurface over the Vulkan surface holding a wl_shm
 *             ARGB8888 buffer (pack() with the ARGB8888 masks).
 *
 * Nothing here moves a window either: hitTest() says "this press is a drag /
 * a resize / a button", and the glue routes it (an app-owned snapped drag on
 * X11, xdg_toplevel.move / .resize on Wayland).
 *
 * THE BAR IS OUTSIDE THE 3D VIEWPORT. Whatever the glue does, the window /
 * surface bound to the runtime must be the content rect only, so the canvas,
 * the Kooima projection, the swapchain and the atlas all exclude the bar and
 * the weave never lands on a chrome pixel.
 *
 * ROUNDED CORNERS. rasterize() gives the top corners an anti-aliased radius
 * (12 logical px, GNOME's) with alpha 0 outside it, PREMULTIPLIED. So the
 * corners only read as rounded where the surface carries alpha — a Wayland
 * ARGB8888 buffer, or an X11 32-bit ARGB visual under a compositing manager.
 * On an opaque X11 visual turn them off (setRoundCorners(false)): premultiplied
 * alpha 0 is black there. The BOTTOM corners belong to the woven content
 * surface and are deliberately square — rounding them would need alpha in the
 * weave output and would clip content.
 *
 * Portable C++17, no dependencies beyond the vendored stb_truetype.h.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dxr_csd {

/*!
 * What a point in the bar is.
 *
 * `Outside` rather than `None`: <X11/X.h> #defines None, which would mangle
 * the enumerator in any TU that also includes Xlib.
 */
enum class Hit
{
	Outside,
	Drag,
	Minimize,
	Close,
	ResizeTop,      //!< thin band along the bar's top edge
	ResizeTopLeft,  //!< top-left corner
	ResizeTopRight, //!< top-right corner
};

//! True for the three Resize* hits.
inline bool
IsResize(Hit h)
{
	return h == Hit::ResizeTop || h == Hit::ResizeTopLeft || h == Hit::ResizeTopRight;
}

/*!
 * Double-click detector, shared so X11 and Wayland agree on what one is.
 * Feed it every primary-button press on the bar's drag area.
 */
class DoubleClick
{
public:
	//! @p time_ms is the window system's event time, @p x/@p y in any one
	//! consistent pixel space, @p slop in that space. True on the second
	//! press of a pair (which also resets the detector).
	bool
	press(uint32_t time_ms, int x, int y, int slop);

	void
	reset()
	{
		armed_ = false;
	}

	//! GTK's default double-click time.
	static constexpr uint32_t kIntervalMs = 400;

private:
	bool armed_ = false;
	uint32_t time_ = 0;
	int x_ = 0, y_ = 0;
};

class TitleBar
{
public:
	//! Bar height in LOGICAL px (libadwaita's header bar).
	static constexpr float kLogicalHeight = 46.0f;
	//! Top-corner radius in LOGICAL px (GNOME / libadwaita windows).
	static constexpr float kLogicalCornerRadius = 12.0f;

	/*!
	 * Set the logical -> device scale and load the title font (once).
	 * Call before height(); call again when the window's output scale
	 * changes. Never fails: without a font the bar has no title.
	 */
	void
	configure(float scale);

	//! Bar height in DEVICE px — kLogicalHeight x scale, rounded up to even
	//! (so an integer-scaled compositor never splits the bar/content seam).
	uint32_t
	height() const
	{
		return height_;
	}
	float
	scale() const
	{
		return scale_;
	}

	//! Effective corner radius in device px: 0 when square (maximised, tiled,
	//! fullscreen, or setRoundCorners(false)).
	uint32_t
	cornerRadius() const;

	//! Classify a point in bar-local DEVICE px, @p winW device px wide.
	//! Outside = not in the bar at all.
	Hit
	hitTest(int x, int y, uint32_t winW) const;

	// ── State. Every change marks the bar dirty; repaint when dirty(). ──────
	void
	setHover(Hit h);
	void
	setPressed(Hit h);
	Hit
	pressed() const
	{
		return pressed_;
	}
	void
	setFocused(bool f);
	//! Maximised / tiled / fullscreen: square corners, no resize edges.
	void
	setMaximized(bool m);
	bool
	maximized() const
	{
		return maximized_;
	}
	//! Whether rounded corners are wanted at all (false on an opaque surface).
	void
	setRoundCorners(bool r);
	//! Whether the top edge offers resize hits (false for a fixed-size window).
	void
	setResizable(bool r);
	void
	setTitle(const std::string &t);
	void
	invalidate()
	{
		dirty_ = true;
	}
	bool
	dirty() const
	{
		return dirty_;
	}

	/*!
	 * Rasterise the bar @p w device px wide if dirty or the width changed,
	 * and clear the dirty flag. The result is PREMULTIPLIED RGBA8, sRGB,
	 * row-major, `w * height() * 4` bytes, alpha 0 outside the corners.
	 */
	const std::vector<uint8_t> &
	render(uint32_t w);

	//! The last render()'s width.
	uint32_t
	renderedWidth() const
	{
		return rasterW_;
	}

	/*!
	 * Pack the last render() into 32-bit pixels whose channels sit at the
	 * given masks (8 bits each). @p dstStrideWords is the row pitch in
	 * uint32_t units. @p aMask 0 = the destination has no alpha channel: the
	 * pixels are written as they would composite over black, and the unused
	 * bits are set to ones (harmless padding on a depth-24 X11 visual).
	 *
	 * wl_shm ARGB8888: r 0x00ff0000, g 0x0000ff00, b 0x000000ff, a 0xff000000.
	 */
	void
	pack(uint32_t *dst,
	     size_t dstStrideWords,
	     uint32_t rMask,
	     uint32_t gMask,
	     uint32_t bMask,
	     uint32_t aMask) const;

	//! The font file in use, empty when none (for the glue's startup log).
	const std::string &
	fontPath() const
	{
		return fontPath_;
	}

private:
	void
	rasterize(uint32_t w);
	void
	loadFont();

	std::vector<uint8_t> pixels_; //!< premultiplied RGBA8, rasterW_ x height_
	uint32_t rasterW_ = 0;

	std::vector<uint8_t> fontData_;
	bool fontTried_ = false;
	bool fontOk_ = false;
	std::string fontPath_;
	std::string title_;

	bool dirty_ = true;
	float scale_ = 1.0f;
	uint32_t height_ = 46;
	Hit hover_ = Hit::Outside;
	Hit pressed_ = Hit::Outside;
	bool focused_ = true;
	bool maximized_ = false;
	bool roundCorners_ = true;
	bool resizable_ = true;
};

} // namespace dxr_csd

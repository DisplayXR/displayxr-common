// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Contract test for csd_titlebar.h (displayxr-common#52).
 *
 * The material and the rounded corners are the parts a reviewer cannot see in
 * a diff and a user sees immediately, so their alpha is pinned here:
 * transparent outside the corner radius, translucent (Style::opacity) in the
 * body, square when maximised, and fully opaque on a surface without alpha.
 * Window-system free — runs on every CI runner.
 */

#include "csd_titlebar.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, msg)                                                                                               \
	do {                                                                                                           \
		if (!(cond)) {                                                                                         \
			std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                          \
			g_failures++;                                                                                  \
		}                                                                                                      \
	} while (0)

using dxr_csd::Hit;
using dxr_csd::TitleBar;

static uint8_t
AlphaAt(const std::vector<uint8_t> &px, uint32_t w, uint32_t x, uint32_t y)
{
	return px[((size_t)y * w + x) * 4 + 3];
}

int
main()
{
	const uint32_t W = 800;

	TitleBar bar;
	bar.configure(2.0f);
	bar.setTitle("DisplayXR");
	CHECK(bar.height() == 92, "46 logical px at scale 2 is 92 device px");
	CHECK(bar.cornerRadius() == 24, "12 logical px radius at scale 2 is 24 device px");

	// Fractional scales round the height up to even.
	{
		TitleBar b;
		b.configure(1.6667f);
		CHECK(b.height() % 2 == 0, "bar height is even at a fractional scale");
	}

	// ── Rounded corners ────────────────────────────────────────────────────
	{
		const std::vector<uint8_t> &px = bar.render(W);
		CHECK(px.size() == (size_t)W * bar.height() * 4, "raster is w x h RGBA8");
		CHECK(!bar.dirty(), "render clears dirty");
		const int bodyA = (int)std::lround(bar.style().opacity * 255.f);
		const int midY = (int)bar.height() / 2;
		CHECK(AlphaAt(px, W, 0, 0) == 0, "top-left corner pixel is transparent");
		CHECK(AlphaAt(px, W, W - 1, 0) == 0, "top-right corner pixel is transparent");
		CHECK(std::abs(AlphaAt(px, W, 60, midY) - bodyA) <= 1, "body is translucent at Style::opacity");
		CHECK(AlphaAt(px, W, 60, midY) < 255, "body is not opaque (the desktop shows through)");
		CHECK(AlphaAt(px, W, W / 2, 0) > AlphaAt(px, W, 60, midY), "top highlight is a denser rim");
		CHECK(AlphaAt(px, W, 0, bar.cornerRadius()) > 0, "left edge below the radius is covered");
		CHECK(AlphaAt(px, W, 0, bar.height() - 1) > 0, "bottom-left is square (content owns it)");
		CHECK(AlphaAt(px, W, W - 1, bar.height() - 1) > 0, "bottom-right is square");
		// Anti-aliased: somewhere in the corner square the alpha is partial.
		bool partial = false;
		for (uint32_t y = 0; y < bar.cornerRadius(); ++y) {
			for (uint32_t x = 0; x < bar.cornerRadius(); ++x) {
				const uint8_t a = AlphaAt(px, W, x, y);
				partial |= (a > 0 && a < 255);
			}
		}
		CHECK(partial, "corner edge is anti-aliased");
		// Opacity is a parameter: a denser material raises the body alpha.
		dxr_csd::Style dense = bar.style();
		dense.opacity = 0.9f;
		TitleBar b2;
		b2.configure(2.0f);
		b2.setStyle(dense);
		CHECK(std::abs(AlphaAt(b2.render(W), W, 60, midY) - (int)std::lround(0.9f * 255.f)) <= 1,
		      "Style::opacity drives the body alpha");
		// Premultiplied: no colour channel exceeds alpha.
		bool premul = true;
		for (size_t i = 0; i < px.size(); i += 4) {
			premul &= px[i] <= px[i + 3] && px[i + 1] <= px[i + 3] && px[i + 2] <= px[i + 3];
		}
		CHECK(premul, "raster is premultiplied");
	}

	// Maximised / switched off -> square.
	bar.setMaximized(true);
	CHECK(bar.dirty(), "maximise marks dirty");
	CHECK(bar.cornerRadius() == 0, "maximised has no radius");
	CHECK(AlphaAt(bar.render(W), W, 0, 0) > 0, "maximised corner is covered (square)");
	bar.setMaximized(false);
	bar.setSurfaceHasAlpha(false);
	CHECK(bar.cornerRadius() == 0, "no alpha -> no radius");
	{
		const std::vector<uint8_t> &px = bar.render(W);
		bool opaque = true;
		for (size_t i = 3; i < px.size(); i += 4) {
			opaque &= px[i] == 255;
		}
		CHECK(opaque, "no alpha -> every pixel opaque (the opaque-visual fallback)");
	}
	bar.setSurfaceHasAlpha(true);

	// ── Hit testing ────────────────────────────────────────────────────────
	CHECK(bar.hitTest(W / 2, bar.height() / 2, W) == Hit::Drag, "bar body is a drag");
	CHECK(bar.hitTest(W / 2, (int)bar.height(), W) == Hit::Outside, "below the bar is outside");
	CHECK(bar.hitTest(-1, 10, W) == Hit::Outside, "left of the bar is outside");
	// Close is the rightmost button: 9 + 12 logical px from the edge.
	CHECK(bar.hitTest((int)W - 42, bar.height() / 2, W) == Hit::Close, "close button");
	CHECK(bar.hitTest((int)W - 42 - 72, bar.height() / 2, W) == Hit::Minimize, "minimize button");
	CHECK(bar.hitTest(W / 2, 2, W) == Hit::ResizeTop, "top band resizes");
	CHECK(bar.hitTest(2, 2, W) == Hit::ResizeTopLeft, "top-left corner resizes");
	CHECK(bar.hitTest((int)W - 3, 2, W) == Hit::ResizeTopRight, "top-right corner resizes");
	bar.setMaximized(true);
	CHECK(bar.hitTest(W / 2, 2, W) == Hit::Drag, "maximised has no resize band");
	bar.setMaximized(false);
	bar.setResizable(false);
	CHECK(bar.hitTest(W / 2, 2, W) == Hit::Drag, "fixed-size window has no resize band");
	bar.setResizable(true);

	// Hover / press only track buttons.
	bar.render(W);
	bar.setHover(Hit::Drag);
	CHECK(!bar.dirty(), "hovering the drag area is not a state change");
	bar.setHover(Hit::Close);
	CHECK(bar.dirty(), "hovering a button repaints");
	bar.setPressed(Hit::ResizeTop);
	CHECK(bar.pressed() == Hit::Outside, "resize is not a pressed button");

	// ── Packing ────────────────────────────────────────────────────────────
	{
		bar.render(W);
		std::vector<uint32_t> argb((size_t)W * bar.height());
		bar.pack(argb.data(), W, 0x00ff0000u, 0x0000ff00u, 0x000000ffu, 0xff000000u);
		CHECK((argb[0] >> 24) == 0, "ARGB8888 corner alpha 0");
		CHECK((argb[(size_t)W * (bar.height() / 2) + 60] >> 24) == (uint32_t)(std::lround(bar.style().opacity * 255.f)),
		      "ARGB8888 body alpha is the material's");
		std::vector<uint32_t> xrgb((size_t)W * bar.height());
		bar.pack(xrgb.data(), W, 0x00ff0000u, 0x0000ff00u, 0x000000ffu, 0u);
		CHECK((xrgb[W / 2] >> 24) == 0xff, "no-alpha visual pads the unused byte with ones");
	}

	// ── Double click ──────────────────────────────────────────────────────
	{
		dxr_csd::DoubleClick dc;
		CHECK(!dc.press(1000, 10, 10, 4), "first press is not a double click");
		CHECK(dc.press(1200, 12, 11, 4), "second press inside the interval is");
		CHECK(!dc.press(1300, 12, 11, 4), "a third press starts a new pair");
		CHECK(!dc.press(2000, 12, 11, 4), "too late");
		CHECK(!dc.press(2100, 40, 11, 4), "too far");
	}

	if (g_failures != 0) {
		std::fprintf(stderr, "csd_test: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("csd_test: OK (font: %s)\n", bar.fontPath().empty() ? "none" : bar.fontPath().c_str());
	return 0;
}

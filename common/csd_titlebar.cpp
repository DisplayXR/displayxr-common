// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Client-side header bar: CPU raster (stb_truetype), window-system
 *         neutral. See csd_titlebar.h for the why.
 */

#include "csd_titlebar.h"

// Private stb_truetype instance: STBTT_STATIC keeps every symbol internal to
// this TU, so a consumer that has its own copy links cleanly.
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dxr_csd {

namespace {

// ── Metrics, in LOGICAL px (x scale_). Approximates a libadwaita header bar:
// 46 px tall, 24 px circular window buttons at the right edge, bold ~11 pt
// title. Close enough not to look broken beside other GNOME windows; this is
// deliberately not a widget toolkit.
constexpr float kBtnR = 12.0f;       // button radius (24 px circles)
constexpr float kBtnRightPad = 9.0f; // bar edge -> close button edge
constexpr float kBtnGap = 12.0f;     // between button edges
constexpr float kBtnHitHalf = 17.0f; // hit square half-size (bigger than the circle, as GTK)
constexpr float kTitlePx = 15.0f;    // bold title pixel height
constexpr float kIconHalf = 4.0f;    // half-extent of the x / - glyphs
constexpr float kIconStroke = 1.3f;  // icon line thickness
constexpr float kResizeBand = 5.0f;  // top-edge resize band height
constexpr float kResizeCorner = 16.0f; // top-corner resize square

// libadwaita dark palette, sRGB. The 3D content below is typically dark, so
// the dark variant is the one that does not look like a hole punched in it.
struct Rgb
{
	float r, g, b;
};
constexpr Rgb kBgFocused = {0x30 / 255.f, 0x30 / 255.f, 0x30 / 255.f};
constexpr Rgb kBgBackdrop = {0x24 / 255.f, 0x24 / 255.f, 0x24 / 255.f};
constexpr Rgb kBorder = {0x1a / 255.f, 0x1a / 255.f, 0x1a / 255.f};
constexpr Rgb kHighlight = {0x3a / 255.f, 0x3a / 255.f, 0x3a / 255.f};
constexpr Rgb kFg = {1.f, 1.f, 1.f};

inline float
Clamp01(float v)
{
	return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
}

inline Rgb
Mix(Rgb a, Rgb b, float t)
{
	return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

//! Distance from p to segment ab.
inline float
SegDist(float px, float py, float ax, float ay, float bx, float by)
{
	const float vx = bx - ax, vy = by - ay;
	const float wx = px - ax, wy = py - ay;
	const float len2 = vx * vx + vy * vy;
	const float t = len2 > 0.f ? Clamp01((wx * vx + wy * vy) / len2) : 0.f;
	const float dx = wx - vx * t, dy = wy - vy * t;
	return std::sqrt(dx * dx + dy * dy);
}

//! Decode UTF-8 into code points (invalid bytes become U+FFFD).
std::vector<int>
Utf8(const std::string &s)
{
	std::vector<int> out;
	for (size_t i = 0; i < s.size();) {
		const unsigned char c = (unsigned char)s[i];
		int cp = 0xFFFD, n = 1;
		if (c < 0x80) {
			cp = c;
		} else if ((c >> 5) == 0x6 && i + 1 < s.size()) {
			cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F);
			n = 2;
		} else if ((c >> 4) == 0xE && i + 2 < s.size()) {
			cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
			n = 3;
		} else if ((c >> 3) == 0x1E && i + 3 < s.size()) {
			cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) |
			     (s[i + 3] & 0x3F);
			n = 4;
		}
		out.push_back(cp);
		i += n;
	}
	return out;
}

bool
ReadFile(const std::string &path, std::vector<uint8_t> &out)
{
	FILE *f = fopen(path.c_str(), "rb");
	if (f == nullptr) {
		return false;
	}
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0) {
		fclose(f);
		return false;
	}
	out.resize((size_t)n);
	const bool ok = fread(out.data(), 1, (size_t)n, f) == (size_t)n;
	fclose(f);
	return ok;
}

// Button centres, in device px, right to left: close, minimize.
struct Layout
{
	float cy, closeCx, minCx, r, hit;
};

Layout
MakeLayout(float scale, uint32_t winW, uint32_t barH)
{
	Layout l;
	l.r = kBtnR * scale;
	l.hit = kBtnHitHalf * scale;
	l.cy = (float)barH * 0.5f;
	l.closeCx = (float)winW - (kBtnRightPad + kBtnR) * scale;
	l.minCx = l.closeCx - (2.f * kBtnR + kBtnGap) * scale;
	return l;
}

//! Coverage of pixel (x, y) by a w-wide shape whose top corners are rounded
//! with radius r (device px). 1 everywhere except inside the two corner
//! squares; analytic, so the edge is anti-aliased.
inline float
CornerCoverage(uint32_t x, uint32_t y, uint32_t w, float r)
{
	if (r <= 0.f || (float)y >= r) {
		return 1.f;
	}
	float cx;
	if ((float)x < r) {
		cx = r;
	} else if ((float)x >= (float)w - r) {
		cx = (float)w - r;
	} else {
		return 1.f;
	}
	const float fx = (float)x + 0.5f, fy = (float)y + 0.5f;
	const float d = std::sqrt((fx - cx) * (fx - cx) + (fy - r) * (fy - r));
	return Clamp01(r - d + 0.5f);
}

int
ShiftOf(uint32_t m)
{
	int s = 0;
	while (m != 0 && (m & 1u) == 0) {
		m >>= 1;
		++s;
	}
	return s;
}

} // namespace


/*
 *
 * DoubleClick
 *
 */

bool
DoubleClick::press(uint32_t time_ms, int x, int y, int slop)
{
	const bool second = armed_ && (uint32_t)(time_ms - time_) <= kIntervalMs && std::abs(x - x_) <= slop &&
	                    std::abs(y - y_) <= slop;
	if (second) {
		armed_ = false;
		return true;
	}
	armed_ = true;
	time_ = time_ms;
	x_ = x;
	y_ = y;
	return false;
}


/*
 *
 * TitleBar
 *
 */

void
TitleBar::loadFont()
{
	fontTried_ = true;
	// DXR_CSD_FONT=/path/to/font.ttf overrides. Otherwise ask fontconfig for
	// the desktop's bold sans (what the GNOME title uses), then fall back to
	// well-known paths. No font is not an error: the bar draws without a title.
	std::vector<std::string> candidates;
	if (const char *e = getenv("DXR_CSD_FONT")) {
		if (e[0] != '\0') {
			candidates.push_back(e);
		}
	}
#if defined(__linux__)
	if (FILE *p = popen("fc-match -f '%{file}' 'sans-serif:bold' 2>/dev/null", "r")) {
		char buf[1024] = {};
		if (fgets(buf, sizeof(buf), p) != nullptr && buf[0] != '\0') {
			candidates.push_back(buf);
		}
		pclose(p);
	}
	candidates.push_back("/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf");
	candidates.push_back("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf");
	candidates.push_back("/usr/share/fonts/opentype/cantarell/Cantarell-Bold.otf");
#elif defined(_WIN32)
	candidates.push_back("C:/Windows/Fonts/segoeuib.ttf");
#elif defined(__APPLE__)
	candidates.push_back("/System/Library/Fonts/Supplemental/Arial Bold.ttf");
#endif
	for (const auto &c : candidates) {
		std::vector<uint8_t> data;
		stbtt_fontinfo info;
		if (ReadFile(c, data) &&
		    stbtt_InitFont(&info, data.data(), stbtt_GetFontOffsetForIndex(data.data(), 0))) {
			fontData_.swap(data);
			fontOk_ = true;
			fontPath_ = c;
			return;
		}
	}
}

void
TitleBar::configure(float scale)
{
	scale_ = (scale >= 0.5f && scale <= 4.0f) ? scale : 1.0f;
	// Even, so an integer-scaled compositor never has to split the
	// bar/content boundary — and so the content's origin stays on an even
	// device pixel whenever the window's does.
	height_ = (uint32_t)std::lround(kLogicalHeight * scale_);
	height_ += height_ & 1u;
	if (!fontTried_) {
		loadFont();
	}
	dirty_ = true;
}

uint32_t
TitleBar::cornerRadius() const
{
	if (!roundCorners_ || maximized_) {
		return 0;
	}
	return (uint32_t)std::lround(kLogicalCornerRadius * scale_);
}

Hit
TitleBar::hitTest(int x, int y, uint32_t winW) const
{
	if (x < 0 || y < 0 || (uint32_t)x >= winW || (uint32_t)y >= height_) {
		return Hit::Outside;
	}
	if (resizable_ && !maximized_) {
		const float corner = kResizeCorner * scale_;
		const float band = kResizeBand * scale_;
		const bool left = (float)x < corner;
		const bool right = (float)x >= (float)winW - corner;
		if ((float)y < band || ((left || right) && (float)y < corner * 0.5f)) {
			return left ? Hit::ResizeTopLeft : right ? Hit::ResizeTopRight : Hit::ResizeTop;
		}
	}
	const Layout l = MakeLayout(scale_, winW, height_);
	auto inBtn = [&](float cx) {
		return std::fabs((float)x - cx) <= l.hit && std::fabs((float)y - l.cy) <= l.hit;
	};
	if (inBtn(l.closeCx)) {
		return Hit::Close;
	}
	if (inBtn(l.minCx)) {
		return Hit::Minimize;
	}
	return Hit::Drag;
}

void
TitleBar::setHover(Hit h)
{
	if (h != Hit::Close && h != Hit::Minimize) {
		h = Hit::Outside; // only buttons have a hover state
	}
	if (h != hover_) {
		hover_ = h;
		dirty_ = true;
	}
}

void
TitleBar::setPressed(Hit h)
{
	if (h != Hit::Close && h != Hit::Minimize) {
		h = Hit::Outside;
	}
	if (h != pressed_) {
		pressed_ = h;
		dirty_ = true;
	}
}

void
TitleBar::setFocused(bool f)
{
	if (f != focused_) {
		focused_ = f;
		dirty_ = true;
	}
}

void
TitleBar::setMaximized(bool m)
{
	if (m != maximized_) {
		maximized_ = m;
		dirty_ = true;
	}
}

void
TitleBar::setRoundCorners(bool r)
{
	if (r != roundCorners_) {
		roundCorners_ = r;
		dirty_ = true;
	}
}

void
TitleBar::setResizable(bool r)
{
	resizable_ = r; // hit testing only; nothing to repaint
}

void
TitleBar::setTitle(const std::string &t)
{
	if (t != title_) {
		title_ = t;
		dirty_ = true;
	}
}

const std::vector<uint8_t> &
TitleBar::render(uint32_t w)
{
	if (w == 0) {
		w = 1;
	}
	if (dirty_ || w != rasterW_) {
		rasterize(w);
		rasterW_ = w;
		dirty_ = false;
	}
	return pixels_;
}

void
TitleBar::rasterize(uint32_t w)
{
	const uint32_t h = height_;
	std::vector<Rgb> px((size_t)w * h);

	const Rgb bg = focused_ ? kBgFocused : kBgBackdrop;
	const Rgb fg = focused_ ? kFg : Mix(bg, kFg, 0.5f);
	std::fill(px.begin(), px.end(), bg);

	const uint32_t line = std::max(1u, (uint32_t)std::lround(scale_));
	// A faint highlight along the top edge (libadwaita draws one inside the
	// rounded window outline), and the shade line separating bar from scene.
	for (uint32_t y = 0; y < line && y < h; ++y) {
		for (uint32_t x = 0; x < w; ++x) {
			px[(size_t)y * w + x] = kHighlight;
		}
	}
	for (uint32_t y = h - line; y < h; ++y) {
		for (uint32_t x = 0; x < w; ++x) {
			px[(size_t)y * w + x] = kBorder;
		}
	}

	const Layout l = MakeLayout(scale_, w, h);

	// Buttons: a faint circle (brighter on hover, brighter still pressed) with
	// a symbolic glyph. Coverage is analytic, so edges are antialiased.
	auto drawButton = [&](float cx, Hit which) {
		float fill = 0.10f;
		if (hover_ == which) {
			fill = 0.15f;
		}
		if (pressed_ == which) {
			fill = 0.30f;
		}
		const float half = kIconHalf * scale_;
		const float stroke = 0.5f * kIconStroke * scale_;
		const int x0 = std::max(0, (int)(cx - l.r - 2)), x1 = std::min((int)w - 1, (int)(cx + l.r + 2));
		const int y0 = std::max(0, (int)(l.cy - l.r - 2)), y1 = std::min((int)h - 1, (int)(l.cy + l.r + 2));
		for (int y = y0; y <= y1; ++y) {
			for (int x = x0; x <= x1; ++x) {
				const float fx = (float)x + 0.5f, fy = (float)y + 0.5f;
				const float d = std::sqrt((fx - cx) * (fx - cx) + (fy - l.cy) * (fy - l.cy));
				const float circle = Clamp01(l.r - d + 0.5f) * fill;
				Rgb &p = px[(size_t)y * w + x];
				p = Mix(p, kFg, circle);
				float glyph;
				if (which == Hit::Close) {
					const float d1 = SegDist(fx, fy, cx - half, l.cy - half, cx + half, l.cy + half);
					const float d2 = SegDist(fx, fy, cx - half, l.cy + half, cx + half, l.cy - half);
					glyph = Clamp01(stroke - std::min(d1, d2) + 0.5f);
				} else {
					const float yy = l.cy + half * 0.75f;
					glyph = Clamp01(stroke - SegDist(fx, fy, cx - half, yy, cx + half, yy) + 0.5f);
				}
				p = Mix(p, fg, glyph);
			}
		}
	};
	if (w > (uint32_t)(2.f * (kBtnRightPad + 2.f * kBtnR + kBtnGap) * scale_)) {
		drawButton(l.closeCx, Hit::Close);
		drawButton(l.minCx, Hit::Minimize);
	}

	// Title, centred on the bar and ellipsized so it never runs under the
	// buttons (symmetric margin, as GTK centres against the whole bar).
	if (fontOk_ && !title_.empty()) {
		stbtt_fontinfo font;
		if (stbtt_InitFont(&font, fontData_.data(), stbtt_GetFontOffsetForIndex(fontData_.data(), 0))) {
			const float fs = stbtt_ScaleForPixelHeight(&font, kTitlePx * scale_);
			int ascent = 0, descent = 0, lineGap = 0;
			stbtt_GetFontVMetrics(&font, &ascent, &descent, &lineGap);
			const float margin = ((float)w - (l.minCx - l.hit)) + 6.f * scale_;
			const float maxW = (float)w - 2.f * margin;

			std::vector<int> cps = Utf8(title_);
			auto measure = [&](const std::vector<int> &c) {
				float adv = 0.f;
				for (size_t i = 0; i < c.size(); ++i) {
					int a = 0, lsb = 0;
					stbtt_GetCodepointHMetrics(&font, c[i], &a, &lsb);
					adv += (float)a * fs;
					if (i + 1 < c.size()) {
						adv += (float)stbtt_GetCodepointKernAdvance(&font, c[i], c[i + 1]) * fs;
					}
				}
				return adv;
			};
			if (maxW <= 0.f) {
				cps.clear();
			}
			while (!cps.empty() && measure(cps) > maxW) {
				if (cps.back() == 0x2026) {
					cps.pop_back();
				}
				if (!cps.empty()) {
					cps.pop_back();
				}
				while (!cps.empty() && cps.back() == ' ') {
					cps.pop_back();
				}
				if (!cps.empty()) {
					cps.push_back(0x2026); // …
				}
				if (cps.size() == 1) {
					cps.clear();
				}
			}
			const float textW = measure(cps);
			float penX = std::floor(((float)w - textW) * 0.5f);
			const float baseline = std::floor(l.cy + (float)(ascent + descent) * fs * 0.5f);
			for (size_t i = 0; i < cps.size(); ++i) {
				int gw = 0, gh = 0, xoff = 0, yoff = 0;
				unsigned char *bmp = stbtt_GetCodepointBitmapSubpixel(
				    &font, fs, fs, penX - std::floor(penX), 0.f, cps[i], &gw, &gh, &xoff, &yoff);
				if (bmp != nullptr) {
					for (int gy = 0; gy < gh; ++gy) {
						const int y = (int)baseline + yoff + gy;
						if (y < 0 || y >= (int)h) {
							continue;
						}
						for (int gx = 0; gx < gw; ++gx) {
							const int x = (int)std::floor(penX) + xoff + gx;
							if (x < 0 || x >= (int)w) {
								continue;
							}
							Rgb &p = px[(size_t)y * w + x];
							p = Mix(p, fg, bmp[gy * gw + gx] / 255.f);
						}
					}
					stbtt_FreeBitmap(bmp, nullptr);
				}
				int a = 0, lsb = 0;
				stbtt_GetCodepointHMetrics(&font, cps[i], &a, &lsb);
				penX += (float)a * fs;
				if (i + 1 < cps.size()) {
					penX += (float)stbtt_GetCodepointKernAdvance(&font, cps[i], cps[i + 1]) * fs;
				}
			}
		}
	}

	// Opaque everywhere except the rounded top corners — including in a
	// transparent-background app: the bar is window chrome, not scene.
	// Premultiplied, so a partially covered corner pixel scales its colour too.
	const float r = (float)cornerRadius();
	pixels_.assign((size_t)w * h * 4, 0);
	for (uint32_t y = 0; y < h; ++y) {
		for (uint32_t x = 0; x < w; ++x) {
			const size_t i = (size_t)y * w + x;
			const float a = CornerCoverage(x, y, w, r);
			pixels_[i * 4 + 0] = (uint8_t)std::lround(Clamp01(px[i].r) * a * 255.f);
			pixels_[i * 4 + 1] = (uint8_t)std::lround(Clamp01(px[i].g) * a * 255.f);
			pixels_[i * 4 + 2] = (uint8_t)std::lround(Clamp01(px[i].b) * a * 255.f);
			pixels_[i * 4 + 3] = (uint8_t)std::lround(a * 255.f);
		}
	}
}

void
TitleBar::pack(uint32_t *dst, size_t dstStrideWords, uint32_t rMask, uint32_t gMask, uint32_t bMask, uint32_t aMask) const
{
	if (dst == nullptr || rasterW_ == 0) {
		return;
	}
	const int rs = ShiftOf(rMask), gs = ShiftOf(gMask), bs = ShiftOf(bMask), as = ShiftOf(aMask);
	const uint32_t fill = aMask != 0 ? 0u : ~(rMask | gMask | bMask);
	for (uint32_t y = 0; y < height_; ++y) {
		uint32_t *row = dst + (size_t)y * dstStrideWords;
		const uint8_t *src = &pixels_[(size_t)y * rasterW_ * 4];
		for (uint32_t x = 0; x < rasterW_; ++x, src += 4) {
			uint32_t v = ((uint32_t)src[0] << rs) | ((uint32_t)src[1] << gs) | ((uint32_t)src[2] << bs);
			v |= aMask != 0 ? ((uint32_t)src[3] << as) : fill;
			row[x] = v;
		}
	}
}

} // namespace dxr_csd

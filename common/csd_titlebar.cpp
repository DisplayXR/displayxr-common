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

// Size metrics live in Style (csd_titlebar.h). Only the resize hit zones are
// fixed: they are about pointer precision, not about the look.
constexpr float kResizeBand = 5.0f;  // top-edge resize band height
constexpr float kResizeCorner = 16.0f; // top-corner resize square

//! One PREMULTIPLIED pixel (sRGB-encoded channels scaled by alpha).
struct Px
{
	float r, g, b, a;
};

inline float
Clamp01(float v)
{
	return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
}

//! Porter-Duff source-over of a flat colour (straight, 0..1) at @p alpha.
inline void
Over(Px &d, float r, float g, float b, float alpha)
{
	const float k = 1.f - alpha;
	d.r = r * alpha + d.r * k;
	d.g = g * alpha + d.g * k;
	d.b = b * alpha + d.b * k;
	d.a = alpha + d.a * k;
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
MakeLayout(const Style &st, float scale, uint32_t winW, uint32_t barH)
{
	Layout l;
	l.r = st.buttonRadius * scale;
	l.hit = st.buttonHitHalf * scale;
	l.cy = (float)barH * 0.5f;
	l.closeCx = (float)winW - (st.buttonRightPad + st.buttonRadius) * scale;
	l.minCx = l.closeCx - (2.f * st.buttonRadius + st.buttonGap) * scale;
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
	height_ = (uint32_t)std::lround((float)logicalHeight() * scale_);
	height_ += height_ & 1u;
	if (!fontTried_) {
		loadFont();
	}
	dirty_ = true;
}

int32_t
TitleBar::logicalHeight() const
{
	const long h = std::lround(style_.barHeight);
	return h < 16 ? 16 : (int32_t)h;
}

uint32_t
TitleBar::cornerRadius() const
{
	if (!hasAlpha_ || maximized_ || style_.cornerRadius <= 0.f) {
		return 0;
	}
	return (uint32_t)std::lround(style_.cornerRadius * scale_);
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
	const Layout l = MakeLayout(style_, scale_, winW, height_);
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
TitleBar::setSurfaceHasAlpha(bool a)
{
	if (a != hasAlpha_) {
		hasAlpha_ = a;
		dirty_ = true;
	}
}

void
TitleBar::setStyle(const Style &s)
{
	style_ = s;
	configure(scale_); // the bar height follows Style::barHeight
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
	const size_t n = (size_t)w * h;
	const Style &st = style_;

	// ── 1. Background material. Translucent dark tint where the surface has
	//    alpha; fully opaque (same tint) where it does not.
	const float bgA = hasAlpha_ ? Clamp01(focused_ ? st.opacity : st.backdropOpacity) : 1.f;
	std::vector<Px> px(n, Px{st.tintR * bgA, st.tintG * bgA, st.tintB * bgA, bgA});

	// ── 2. Edges: a lighter 1 px top highlight (reads as the lit rim of a
	//    glass pane, and outlines the bar over a dark desktop) and a dark 1 px
	//    separator where the bar meets the content.
	const uint32_t line = std::max(1u, (uint32_t)std::lround(scale_));
	for (uint32_t y = 0; y < line && y < h; ++y) {
		for (uint32_t x = 0; x < w; ++x) {
			Over(px[(size_t)y * w + x], 1.f, 1.f, 1.f, st.highlightAlpha);
		}
	}
	for (uint32_t y = h > line ? h - line : 0; y < h; ++y) {
		for (uint32_t x = 0; x < w; ++x) {
			Over(px[(size_t)y * w + x], 0.f, 0.f, 0.f, st.separatorAlpha);
		}
	}

	const Layout l = MakeLayout(style_, scale_, w, h);
	const bool showButtons =
	    w > (uint32_t)(2.f * (st.buttonRightPad + 2.f * st.buttonRadius + st.buttonGap) * scale_);

	// ── 3. Button discs: faint white, brighter hovered, brighter still pressed.
	auto drawDisc = [&](float cx, Hit which) {
		float fill = st.buttonFill;
		if (hover_ == which) {
			fill = st.buttonFillHover;
		}
		if (pressed_ == which) {
			fill = st.buttonFillPressed;
		}
		const int x0 = std::max(0, (int)(cx - l.r - 2)), x1 = std::min((int)w - 1, (int)(cx + l.r + 2));
		const int y0 = std::max(0, (int)(l.cy - l.r - 2)), y1 = std::min((int)h - 1, (int)(l.cy + l.r + 2));
		for (int y = y0; y <= y1; ++y) {
			for (int x = x0; x <= x1; ++x) {
				const float fx = (float)x + 0.5f, fy = (float)y + 0.5f;
				const float d = std::sqrt((fx - cx) * (fx - cx) + (fy - l.cy) * (fy - l.cy));
				const float cov = Clamp01(l.r - d + 0.5f);
				if (cov > 0.f) {
					Over(px[(size_t)y * w + x], 1.f, 1.f, 1.f, cov * fill);
				}
			}
		}
	};
	if (showButtons) {
		drawDisc(l.closeCx, Hit::Close);
		drawDisc(l.minCx, Hit::Minimize);
	}

	// ── 4. Foreground coverage (glyphs + title) into one mask, so the soft
	//    shadow below it is computed once for everything legible.
	std::vector<float> fg(n, 0.f);
	auto plot = [&](int x, int y, float c) {
		if (x >= 0 && y >= 0 && x < (int)w && y < (int)h && c > 0.f) {
			float &m = fg[(size_t)y * w + x];
			m = std::max(m, Clamp01(c));
		}
	};
	auto drawGlyph = [&](float cx, Hit which) {
		const float half = st.iconHalf * scale_;
		const float stroke = 0.5f * st.iconStroke * scale_;
		const int x0 = (int)(cx - half - stroke - 2), x1 = (int)(cx + half + stroke + 2);
		const int y0 = (int)(l.cy - half - stroke - 2), y1 = (int)(l.cy + half + stroke + 2);
		for (int y = y0; y <= y1; ++y) {
			for (int x = x0; x <= x1; ++x) {
				const float fx = (float)x + 0.5f, fy = (float)y + 0.5f;
				float d;
				if (which == Hit::Close) {
					d = std::min(SegDist(fx, fy, cx - half, l.cy - half, cx + half, l.cy + half),
					             SegDist(fx, fy, cx - half, l.cy + half, cx + half, l.cy - half));
				} else {
					const float yy = l.cy + half * 0.75f;
					d = SegDist(fx, fy, cx - half, yy, cx + half, yy);
				}
				plot(x, y, stroke - d + 0.5f);
			}
		}
	};
	if (showButtons) {
		drawGlyph(l.closeCx, Hit::Close);
		drawGlyph(l.minCx, Hit::Minimize);
	}

	// Title, centred on the bar and ellipsized so it never runs under the
	// buttons (symmetric margin, as GTK centres against the whole bar).
	if (fontOk_ && !title_.empty()) {
		stbtt_fontinfo font;
		if (stbtt_InitFont(&font, fontData_.data(), stbtt_GetFontOffsetForIndex(fontData_.data(), 0))) {
			const float fs = stbtt_ScaleForPixelHeight(&font, st.titlePx * scale_);
			int ascent = 0, descent = 0, lineGap = 0;
			stbtt_GetFontVMetrics(&font, &ascent, &descent, &lineGap);
			const float margin = ((float)w - (l.minCx - l.hit)) + 6.f * scale_;
			const float maxW = (float)w - 2.f * margin;

			std::vector<int> cps = Utf8(title_);
			auto measure = [&](const std::vector<int> &c) {
				float adv = 0.f;
				for (size_t i = 0; i < c.size(); ++i) {
					int adv_ = 0, lsb = 0;
					stbtt_GetCodepointHMetrics(&font, c[i], &adv_, &lsb);
					adv += (float)adv_ * fs;
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
						for (int gx = 0; gx < gw; ++gx) {
							plot((int)std::floor(penX) + xoff + gx, (int)baseline + yoff + gy,
							     bmp[gy * gw + gx] / 255.f);
						}
					}
					stbtt_FreeBitmap(bmp, nullptr);
				}
				int adv_ = 0, lsb = 0;
				stbtt_GetCodepointHMetrics(&font, cps[i], &adv_, &lsb);
				penX += (float)adv_ * fs;
				if (i + 1 < cps.size()) {
					penX += (float)stbtt_GetCodepointKernAdvance(&font, cps[i], cps[i + 1]) * fs;
				}
			}
		}
	}

	// ── 5. Soft text shadow: the foreground mask, dropped by
	//    textShadowOffset and box-blurred (radius ~1 logical px), in black.
	//    It is what keeps white text legible where a light desktop shows
	//    through the translucent material.
	if (st.textShadowAlpha > 0.f) {
		const int drop = std::max(1, (int)std::lround(st.textShadowOffset * scale_));
		const int rad = std::max(1, (int)std::lround(scale_));
		std::vector<float> tmp(n, 0.f), sh(n, 0.f);
		for (uint32_t y = 0; y < h; ++y) { // horizontal box
			float acc = 0.f;
			const int win = 2 * rad + 1;
			for (int x = -rad; x < (int)w; ++x) {
				const int add = x + rad, sub = x - rad - 1;
				if (add < (int)w) {
					acc += fg[(size_t)y * w + add];
				}
				if (sub >= 0) {
					acc -= fg[(size_t)y * w + sub];
				}
				if (x >= 0) {
					tmp[(size_t)y * w + x] = acc / (float)win;
				}
			}
		}
		for (uint32_t x = 0; x < w; ++x) { // vertical box, with the drop
			for (uint32_t y = 0; y < h; ++y) {
				float acc = 0.f;
				for (int k = -rad; k <= rad; ++k) {
					const int sy = (int)y - drop + k;
					if (sy >= 0 && sy < (int)h) {
						acc += tmp[(size_t)sy * w + x];
					}
				}
				sh[(size_t)y * w + x] = acc / (float)(2 * rad + 1);
			}
		}
		for (size_t i = 0; i < n; ++i) {
			if (sh[i] > 0.f) {
				Over(px[i], 0.f, 0.f, 0.f, Clamp01(sh[i]) * st.textShadowAlpha);
			}
		}
	}

	// ── 6. The foreground itself: white, dimmed on an unfocused window.
	const float fgA = focused_ ? 1.f : st.backdropTextAlpha;
	for (size_t i = 0; i < n; ++i) {
		if (fg[i] > 0.f) {
			Over(px[i], 1.f, 1.f, 1.f, fg[i] * fgA);
		}
	}

	// ── 7. Rounded top corners: scale the whole premultiplied pixel by the
	//    corner coverage (alpha 0 outside the radius, anti-aliased edge). With
	//    no alpha on the surface, force every pixel opaque instead.
	const float r = (float)cornerRadius();
	pixels_.assign(n * 4, 0);
	for (uint32_t y = 0; y < h; ++y) {
		for (uint32_t x = 0; x < w; ++x) {
			const size_t i = (size_t)y * w + x;
			Px p = px[i];
			if (!hasAlpha_) {
				// Composite over black (the premultiplied colour already is).
				p.a = 1.f;
			} else {
				const float c = CornerCoverage(x, y, w, r);
				p.r *= c;
				p.g *= c;
				p.b *= c;
				p.a *= c;
			}
			pixels_[i * 4 + 0] = (uint8_t)std::lround(Clamp01(p.r) * 255.f);
			pixels_[i * 4 + 1] = (uint8_t)std::lround(Clamp01(p.g) * 255.f);
			pixels_[i * 4 + 2] = (uint8_t)std::lround(Clamp01(p.b) * 255.f);
			pixels_[i * 4 + 3] = (uint8_t)std::lround(Clamp01(p.a) * 255.f);
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

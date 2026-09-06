// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief App-side content-MASK ROI helpers for `XR_DXR_depth_budget` v3
 *        (`XrContentMaskDXR`) — rear-depth-budget design brief §6.
 *
 * v2 (`content_bounds.h`) narrowed the runtime's rear-depth analysis from the
 * whole canvas to a RECT around the app's content. A rect is still mostly
 * background: a zone-clamped box around a character is ~2/3 pixels the model
 * never covers, and any horizontal structure sitting in that surplus closes
 * the clip for content that never overlapped it. v3 hands the runtime the
 * SILHOUETTE instead — the union over all views of where the app's content
 * actually lands — as a small occupancy grid chained on `XrFrameEndInfo::next`.
 *
 * The app does not have to produce anything new for this. A transparent app
 * already derives exactly this artefact every frame, from its own rendered
 * alpha, to build its click-through window region (`vk_clickthrough_region.h`
 * on Windows/Vulkan; the Unity overlay's union-of-L/R silhouette; the demos'
 * equivalents). `ContentMaskFromCoverage()` is a max-filter (any-coverage)
 * downsample of that existing coverage buffer onto the extension's cell grid;
 * `ChainContentMask()` is the pointer bookkeeping that attaches it.
 *
 * Division of labour, unchanged: the runtime owns the rear-depth POLICY, the
 * display processor owns PIXELS, the app owns GEOMETRY. Everything here is
 * pure data reshaping — no OpenXR calls, no global state, no logging, no
 * platform dependency (this header is part of `displayxr::rules`). In
 * particular the app does NOT dilate, does NOT clamp to its 3D zones and does
 * NOT smooth: the runtime resamples the mask onto its own analysis grid,
 * clamps it to the 3D zones and dilates it by the disparity band before
 * measuring. Producing an already-dilated mask here would compound with that.
 *
 * ## Grid sizing
 *
 * The extension allows 1..512 cells per side. The recommendation is **256x256
 * maximum**, and in practice the app's own coverage buffer downsampled by
 * **4-8x** — e.g. `ClickThroughRegion`'s 4-px-per-texel raster of a 1920x1080
 * window is 480x270 texels, so a 120x68 or 60x34 grid. Finer than that buys
 * nothing: the runtime dilates the mask by ~4% of the preview width (min 8 px)
 * before it measures, which erases sub-cell detail anyway, and every extra cell
 * is bytes copied inside `xrEndFrame`.
 *
 * ## Grid convention
 *
 * Row-major, top-left origin, **window-client-normalised**: cell `(x, y)`
 * covers `[x/width, (x+1)/width) x [y/height, (y+1)/height)` of the app
 * window's CLIENT rect — the same frame as `XrContentBoundsDXR::bounds` after
 * `RebaseZoneBoundsToWindow()`. A zoned app therefore writes its zone's
 * silhouette into the window grid and leaves everything outside the zone at 0;
 * `ContentMaskFromCoverage()`'s `srcRectPx` parameter does that placement.
 *
 * Header-dependency note: `XrContentMaskDXR` and `XR_TYPE_CONTENT_MASK_DXR`
 * ship in `XR_DXR_depth_budget.h` SPEC_VERSION 3. When the pinned extensions
 * header is older (SPEC_VERSION 2 has neither), this file defines an
 * ABI-identical local fallback (guarded by `DXR_CONTENT_MASK_LOCAL_DEF`) so
 * callers can build today — the same pattern `content_bounds.h` used for v2.
 * Once the pin advances to SPEC_VERSION >= 3 the fallback compiles out and the
 * header's own definition is used, with no call-site changes required.
 *
 * Usage (once per frame, after the frame's silhouette/coverage is available):
 *
 *     std::vector<uint8_t> cells;
 *     dxr::ContentMaskFromCoverage(cov, covW, covH, covW,
 *                                  windowW, windowH, nullptr,   // whole window
 *                                  64, 64, cells);
 *     if (dxr::ContentMaskCoverageCells(cells) != 0) {
 *         XrContentMaskDXR mask;
 *         dxr::ChainContentMask(frameEndInfo, mask, cells, 64, 64);
 *     }
 *     xrEndFrame(session, &frameEndInfo);
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <openxr/openxr.h>
#include <openxr/XR_DXR_depth_budget.h>

#if !defined(XR_DXR_depth_budget_SPEC_VERSION) || (XR_DXR_depth_budget_SPEC_VERSION < 3)
// The pinned extensions header predates the v3 bump. Unlike v2 — whose type
// value was reserved a version early — SPEC_VERSION 2 carries NEITHER
// XR_TYPE_CONTENT_MASK_DXR nor XrContentMaskDXR, so both are defined here.
// Values are taken verbatim from the v3 header (brief §6.1); the struct is
// field-for-field ABI-identical, so once the pin advances this whole block
// compiles out and the real definitions are used with no caller changes.
#ifndef DXR_CONTENT_MASK_LOCAL_DEF
#define DXR_CONTENT_MASK_LOCAL_DEF
#ifndef XR_TYPE_CONTENT_MASK_DXR
#define XR_TYPE_CONTENT_MASK_DXR ((XrStructureType)1004999263)
#endif
typedef struct XrContentMaskDXR {
    XrStructureType type;             //!< Must be XR_TYPE_CONTENT_MASK_DXR
    const void*     next;
    uint32_t        width;            //!< 1..512 cells
    uint32_t        height;           //!< 1..512 cells
    uint32_t        strideBytes;      //!< >= width
    const uint8_t*  cells;            //!< width x height bytes, nonzero = occupied
    float           marginNormalized; //!< Extra dilation the app wants; 0 = runtime default
} XrContentMaskDXR;
#endif // DXR_CONTENT_MASK_LOCAL_DEF
#endif // SPEC_VERSION < 3

namespace dxr {

//! The extension's per-side cell-count ceiling (`XrContentMaskDXR::width`/
//! `height` are documented as 1..512). A grid outside this range is rejected
//! rather than silently clamped: the runtime would reject it too, and a
//! silently-shrunk mask is a wrong ROI, not a smaller one.
static constexpr uint32_t kContentMaskMaxCells = 512;

//! Recommended per-side ceiling (see the file comment): the runtime's own
//! dilation erases anything finer, so this is where extra cells stop paying.
static constexpr uint32_t kContentMaskRecommendedCells = 256;

namespace detail {
// Integer min/max/division helpers. Deliberately NOT std::min/std::max and NOT
// named MinF/MaxF: a TU that also pulls in <windows.h> (window_manager.h,
// vk_clickthrough_region.h) without NOMINMAX gets `min`/`max` function-like
// macros that mangle `std::min(`/`std::max(` at the preprocessor stage, and
// content_bounds.h already owns the MinF/MaxF names in this namespace.
inline int64_t MinI64(int64_t a, int64_t b) { return a < b ? a : b; }
inline int64_t MaxI64(int64_t a, int64_t b) { return a > b ? a : b; }

//! floor(a / b) for b > 0, correct for negative a (C++ integer division
//! truncates toward zero — a source rect placed partly off the left/top edge
//! of the window has a negative numerator).
inline int64_t
FloorDivI64(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if ((a % b) != 0 && ((a < 0) != (b < 0))) {
        --q;
    }
    return q;
}

//! ceil(a / b) for b > 0, correct for negative a.
inline int64_t
CeilDivI64(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if ((a % b) != 0 && ((a < 0) == (b < 0))) {
        ++q;
    }
    return q;
}

/*!
 * Cell index range `[lo, hi)` touched by source sample `i` of `srcN` samples,
 * where the source spans window pixels `[rectOff, rectOff + rectExtent)` and
 * the grid divides window pixels `[0, windowPx)` into `outN` cells.
 *
 * Exact rational integer arithmetic throughout (no floating point): a sample
 * boundary that lands exactly on a cell boundary must NOT be pushed into the
 * next cell by an epsilon, or a blob that exactly fills a cell would smear
 * into its neighbours. Returns false when the sample falls entirely outside
 * the window.
 */
inline bool
CellRangeForSample(uint32_t i, uint32_t srcN, int32_t rectOff, int32_t rectExtent,
                   uint32_t windowPx, uint32_t outN, int64_t* lo, int64_t* hi)
{
    // Window-pixel span of this sample, scaled by srcN to stay integral:
    //   wx0 * srcN = rectOff * srcN + i       * rectExtent
    //   wx1 * srcN = rectOff * srcN + (i + 1) * rectExtent
    const int64_t srcNi = (int64_t)srcN;
    const int64_t base = (int64_t)rectOff * srcNi;
    const int64_t n0 = base + (int64_t)i * (int64_t)rectExtent;
    const int64_t n1 = base + (int64_t)(i + 1) * (int64_t)rectExtent;

    // Entirely off the left/top or right/bottom of the window client rect.
    if (n1 <= 0 || n0 >= (int64_t)windowPx * srcNi) {
        return false;
    }

    // cell = wx * outN / windowPx = n * outN / (srcN * windowPx)
    const int64_t den = srcNi * (int64_t)windowPx;
    int64_t c0 = FloorDivI64(n0 * (int64_t)outN, den);
    int64_t c1 = CeilDivI64(n1 * (int64_t)outN, den);

    c0 = MaxI64(c0, 0);
    c0 = MinI64(c0, (int64_t)outN - 1);
    c1 = MinI64(c1, (int64_t)outN);
    if (c1 <= c0) {
        c1 = c0 + 1; // a sample smaller than a cell still marks the cell it lands in
    }

    *lo = c0;
    *hi = c1;
    return true;
}
} // namespace detail

/*!
 * Max-filter (ANY-coverage) downsample of a coverage/alpha buffer onto the
 * `XrContentMaskDXR` cell grid.
 *
 * The source is a coverage buffer — 1 byte per pixel, **nonzero = covered**,
 * row-major, top-left origin — of size `srcW x srcH` with `srcStride` bytes
 * per row. This is deliberately the shape of what a transparent app already
 * has: `ClickThroughRegion::coverage()` hands back exactly this (see
 * `vk_clickthrough_region.h`), and an app holding raw alpha instead should
 * threshold it into a byte buffer first — "nonzero" here is taken literally,
 * so a 1/255 anti-aliased fringe counts as covered.
 *
 * ANY-coverage is the whole point of the filter: a cell is marked if ANY
 * overlapping source pixel is covered — a single covered pixel marks its
 * cell. Erring outward is correct here, because the runtime measures the
 * background only INSIDE the mask; a cell wrongly cleared hides a real
 * conflict, a cell wrongly set at worst measures a little extra background.
 *
 * Placement: by default the source is taken to cover the WHOLE window client
 * rect. Pass `srcRectPx` (window client pixels, top-left origin) to place it
 * somewhere else instead — the zoned case, where the coverage was rendered for
 * one `XR_DXR_display_zones` 3D zone. Cells outside that rect stay 0, which is
 * exactly the "leave the rest of the window unmasked" contract the extension
 * wants; the part of the rect that falls outside the window is dropped.
 *
 * @param src        Coverage buffer, `srcH` rows of `srcStride` bytes.
 * @param srcW       Coverage width in samples.
 * @param srcH       Coverage height in samples.
 * @param srcStride  Bytes per coverage row; must be >= `srcW`.
 * @param windowW    Window CLIENT width in pixels (the grid's frame).
 * @param windowH    Window CLIENT height in pixels.
 * @param srcRectPx  Optional: where the coverage sits in window client pixels.
 *                    `nullptr` (or a rect with extent <= 0 is rejected) means
 *                    the coverage covers the whole window.
 * @param outW       Grid width in cells, 1..512.
 * @param outH       Grid height in cells, 1..512.
 * @param out        Resized to `outW * outH` and filled with 0 / 255.
 *
 * @return true when the input was well-formed. **A valid but fully-uncovered
 *         source is NOT a failure**: it returns true with an all-zero grid, and
 *         the caller should check `ContentMaskCoverageCells()` before chaining
 *         (the runtime treats an all-zero mask as absent and falls back to the
 *         content bounds). false means degenerate input — null `src`, a zero
 *         dimension, `srcStride < srcW`, a non-positive `srcRectPx` extent, or
 *         a grid outside 1..512 — and `out` is left all-zero (sized
 *         `outW * outH` when those dims are themselves usable, else empty).
 */
inline bool
ContentMaskFromCoverage(const uint8_t* src, uint32_t srcW, uint32_t srcH, uint32_t srcStride,
                        uint32_t windowW, uint32_t windowH, const XrRect2Di* srcRectPx,
                        uint32_t outW, uint32_t outH, std::vector<uint8_t>& out)
{
    const bool gridUsable = outW >= 1 && outW <= kContentMaskMaxCells && outH >= 1 &&
                            outH <= kContentMaskMaxCells;
    if (gridUsable) {
        out.assign((size_t)outW * outH, 0);
    } else {
        out.clear();
        return false;
    }

    if (src == nullptr || srcW == 0 || srcH == 0 || srcStride < srcW || windowW == 0 ||
        windowH == 0) {
        return false;
    }

    // Where the coverage sits in window client pixels.
    int32_t rectX = 0, rectY = 0;
    int32_t rectW = (int32_t)windowW, rectH = (int32_t)windowH;
    if (srcRectPx != nullptr) {
        if (srcRectPx->extent.width <= 0 || srcRectPx->extent.height <= 0) {
            return false;
        }
        rectX = srcRectPx->offset.x;
        rectY = srcRectPx->offset.y;
        rectW = srcRectPx->extent.width;
        rectH = srcRectPx->extent.height;
    }

    // Column ranges are the same for every row — compute them once.
    std::vector<int64_t> colLo((size_t)srcW), colHi((size_t)srcW);
    for (uint32_t sx = 0; sx < srcW; ++sx) {
        int64_t lo = 0, hi = 0;
        if (!detail::CellRangeForSample(sx, srcW, rectX, rectW, windowW, outW, &lo, &hi)) {
            lo = 0;
            hi = 0; // empty range: this column is off-window
        }
        colLo[sx] = lo;
        colHi[sx] = hi;
    }

    // One scratch row of cell flags per source row, then OR'd into every grid
    // row the source row overlaps. Cheaper than re-scanning the source once
    // per destination row when the grid is coarser than the coverage.
    std::vector<uint8_t> rowFlags((size_t)outW, 0);

    for (uint32_t sy = 0; sy < srcH; ++sy) {
        int64_t cy0 = 0, cy1 = 0;
        if (!detail::CellRangeForSample(sy, srcH, rectY, rectH, windowH, outH, &cy0, &cy1)) {
            continue; // this row is off-window
        }

        const uint8_t* srow = src + (size_t)sy * srcStride;
        bool any = false;
        std::memset(rowFlags.data(), 0, rowFlags.size());
        for (uint32_t sx = 0; sx < srcW; ++sx) {
            if (srow[sx] == 0) {
                continue;
            }
            const int64_t lo = colLo[sx], hi = colHi[sx];
            if (hi <= lo) {
                continue;
            }
            std::memset(rowFlags.data() + lo, 1, (size_t)(hi - lo));
            any = true;
        }
        if (!any) {
            continue;
        }

        for (int64_t cy = cy0; cy < cy1; ++cy) {
            uint8_t* orow = out.data() + (size_t)cy * outW;
            for (uint32_t cx = 0; cx < outW; ++cx) {
                if (rowFlags[cx] != 0) {
                    orow[cx] = 255;
                }
            }
        }
    }

    return true;
}

/*!
 * Union `b` into `a` (per-cell OR), for masks of identical dimensions.
 *
 * The use is compositing sources the app derives separately — a 3D zone's
 * silhouette plus a second zone's, or a silhouette plus a chrome band the app
 * knows carries content. Both grids must already be in the SAME
 * window-normalised frame at the same `outW x outH`; a size mismatch is a
 * no-op (there is no meaningful frame to resample into here — build both with
 * the same `outW`/`outH` via `ContentMaskFromCoverage()`).
 *
 * Cells are normalised to 0 / 255 on the way out, so a union of grids from
 * different producers stays canonical.
 */
inline void
ContentMaskUnion(std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    if (a.size() != b.size()) {
        return;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        a[i] = (a[i] != 0 || b[i] != 0) ? (uint8_t)255 : (uint8_t)0;
    }
}

/*!
 * Number of occupied (nonzero) cells.
 *
 * Check this before chaining: the runtime treats an all-zero mask as ABSENT
 * and falls back to `XrContentBoundsDXR` (then the 3D zones, then the whole
 * canvas), so chaining a zero mask is not an error but it is also not what an
 * app that had nothing to draw this frame usually wants to say.
 */
inline uint32_t
ContentMaskCoverageCells(const std::vector<uint8_t>& cells)
{
    uint32_t n = 0;
    for (size_t i = 0; i < cells.size(); ++i) {
        if (cells[i] != 0) {
            ++n;
        }
    }
    return n;
}

/*!
 * Zero-init `out`, stamp `XR_TYPE_CONTENT_MASK_DXR`, point it at `cells`, and
 * link it onto `fei.next`, preserving whatever chain `XrFrameEndInfo` already
 * carries. Call once per frame before `xrEndFrame`.
 *
 * **`cells` must outlive the `xrEndFrame` call** — the struct stores a pointer,
 * not a copy (the runtime copies during `xrEndFrame`). A `std::vector` that
 * lives across frames, or is a member of the app's frame state, is the
 * intended shape; a temporary is not.
 *
 * Chains cleanly beside `ChainContentBounds()`. Chain the MASK FIRST, then the
 * bounds: `fei.next` then points at the bounds, whose `next` points at the
 * mask, whose `next` is whatever was there before — both reachable, and the
 * runtime's own precedence (mask, then bounds) is independent of chain order.
 *
 * @return false — WITHOUT touching `fei` — when the mask could not be
 *         described: `width`/`height` outside 1..512, or `cells` shorter than
 *         `width * height`. Otherwise true. An all-zero (but correctly sized)
 *         mask is chained happily; the runtime reads it as "absent".
 */
inline bool
ChainContentMask(XrFrameEndInfo& fei, XrContentMaskDXR& out, const std::vector<uint8_t>& cells,
                 uint32_t w, uint32_t h, float marginNormalized = 0.0f)
{
    if (w < 1 || w > kContentMaskMaxCells || h < 1 || h > kContentMaskMaxCells) {
        return false;
    }
    if (cells.size() < (size_t)w * h) {
        return false;
    }

    out = XrContentMaskDXR{};
    out.type = (XrStructureType)XR_TYPE_CONTENT_MASK_DXR;
    out.next = fei.next;
    out.width = w;
    out.height = h;
    out.strideBytes = w; // ContentMaskFromCoverage produces tightly-packed rows
    out.cells = cells.data();
    out.marginNormalized = marginNormalized;
    fei.next = &out;
    return true;
}

} // namespace dxr

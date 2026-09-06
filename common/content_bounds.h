// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief App-side content-bounds ROI helpers for `XR_DXR_depth_budget` v2
 *        (`XrContentBoundsDXR`) — rear-depth-budget design brief §5.
 *
 * v1 of the rear-depth-budget analysis (see `clip_policy.h`) looks at the
 * WHOLE canvas for a horizontal-disparity cue. That over-analyses: an app's
 * actual content usually covers a fraction of the canvas, and busy pixels
 * outside that fraction (a window's menu bar, a taskbar) can needlessly close
 * the budget even though they never sit behind the app's own geometry. v2
 * lets the app narrow the runtime's analysis region to where its content
 * actually projects, by chaining `XrContentBoundsDXR` on `XrFrameEndInfo::next`
 * in `xrEndFrame`.
 *
 * Division of labour, unchanged: the runtime owns the rear-depth POLICY, the
 * display processor owns PIXELS, the app owns GEOMETRY. `ProjectAabbToCanvasBounds`
 * is the app-side geometry step — pure math, no OpenXR calls, no global state,
 * no logging — that turns a world-space content AABB into the canvas-normalised
 * rect the extension struct wants. `ChainContentBounds` is pointer bookkeeping
 * to attach the result to the frame.
 *
 * Header-dependency note: `XrContentBoundsDXR` is reserved (its `type` value,
 * `XR_TYPE_CONTENT_BOUNDS_DXR`) in `XR_DXR_depth_budget.h` SPEC_VERSION 1, but
 * the struct itself ships in SPEC_VERSION 2. When the pinned extensions header
 * is older than that bump, this file defines an ABI-identical local fallback
 * (guarded by `DXR_CONTENT_BOUNDS_LOCAL_DEF`) so callers can build today; once
 * the pin advances to SPEC_VERSION >= 2 the fallback compiles out and the
 * header's own definition is used, with no call-site changes required.
 *
 * Usage (once per frame, after the view/proj matrices are built):
 *
 *     const float* viewProj[2] = { leftViewProjColMajor, rightViewProjColMajor };
 *     XrRect2Df bounds{};
 *     dxr::ProjectAabbToCanvasBounds(aabbMin, aabbMax, viewProj, 2, &bounds);
 *
 *     XrContentBoundsDXR contentBounds;
 *     dxr::ChainContentBounds(frameEndInfo, contentBounds, bounds);
 *     xrEndFrame(session, &frameEndInfo);
 */
#pragma once

#include <cmath>
#include <cstdint>

#include <openxr/openxr.h>
#include <openxr/XR_DXR_depth_budget.h>

#if !defined(XR_DXR_depth_budget_SPEC_VERSION) || (XR_DXR_depth_budget_SPEC_VERSION < 2)
// The pinned extensions header predates the v2 bump: XR_TYPE_CONTENT_BOUNDS_DXR
// is reserved there already (SPEC_VERSION 1), but XrContentBoundsDXR itself is
// not. Define an ABI-identical stand-in — same field order/types as brief §5.1
// — so this file's API is usable against the older pin. Once the pin advances
// to SPEC_VERSION >= 2 this whole block compiles out and the real header's
// struct (byte-identical) is used instead; no caller changes needed.
#ifndef DXR_CONTENT_BOUNDS_LOCAL_DEF
#define DXR_CONTENT_BOUNDS_LOCAL_DEF
typedef struct XrContentBoundsDXR {
    XrStructureType type; //!< Must be XR_TYPE_CONTENT_BOUNDS_DXR
    const void*     next;
    XrRect2Df       bounds;           //!< Canvas-normalised, origin top-left, v down
    float           marginNormalized; //!< Extra dilation the app wants; 0 = runtime default
} XrContentBoundsDXR;
#endif // DXR_CONTENT_BOUNDS_LOCAL_DEF
#endif // SPEC_VERSION < 2

namespace dxr {

namespace detail {
// Plain min/max/clamp — deliberately NOT std::min/std::max: a TU that also
// pulls in <windows.h> (e.g. window_manager.h) without NOMINMAX gets `min`/
// `max` function-like macros that mangle `std::min(`/`std::max(` into a
// syntax error at the preprocessor stage.
inline float MinF(float a, float b) { return a < b ? a : b; }
inline float MaxF(float a, float b) { return a > b ? a : b; }
inline float Clamp01F(float v) { return MinF(MaxF(v, 0.0f), 1.0f); }
} // namespace detail

/*!
 * Project the 8 corners of a world-space axis-aligned bounding box through
 * each eye's column-major 4x4 view-projection matrix, union the results over
 * eyes, and express the union as a rect normalised to the VIEW's own canvas —
 * i.e. `viewProjPerEye` fully determines the normalisation frame. For a
 * window-filling view that IS the app window's client rect. For a zone-scoped
 * locate (the view-proj of a single `XR_DXR_display_zones` 3D zone, e.g. the
 * bottom-band zone of a mixed 2D/3D layout) the output is normalised to that
 * ZONE, not the window — chaining it into `XrContentBoundsDXR::bounds`
 * unchanged would scale the runtime's analysis region onto the whole window
 * and reach into any 2D band outside the zone. Zoned apps must rebase the
 * zone-normalised result into window-normalised space before chaining it —
 * use `RebaseZoneBoundsToWindow` (or the one-call `ProjectAabbToWindowBounds`)
 * for that step.
 *
 * Convention: NDC (x,y) in [-1,1] -> canvas-normalised (u,v) via
 * `u = (x+1)/2`, `v = (1-y)/2` — origin top-left, v DOWN, the same convention
 * as the background-preview `canvas_u0..v1` and `XrViewDisplayRawDXR::canvasRectPx`.
 *
 * Conservative on failure: if any of the 8*eyeCount projected corners has
 * `w <= 0` (behind the eye — the matrix cannot be trusted for that corner),
 * this returns false and writes the whole canvas ({0,0,1,1}) to `*out`, never
 * a partial or garbage rect.
 *
 * @param aabbMin        World-space AABB min corner, float[3].
 * @param aabbMax        World-space AABB max corner, float[3].
 * @param viewProjPerEye Array of `eyeCount` pointers, each a column-major 4x4
 *                       (16-float) view-projection matrix for that eye.
 * @param eyeCount       Number of eyes / entries in `viewProjPerEye`.
 * @param out            Output rect. Always written (whole canvas on failure).
 * @return true on success (all corners in front of every eye), false otherwise
 *         (including null/degenerate input).
 */
inline bool
ProjectAabbToCanvasBounds(const float aabbMin[3], const float aabbMax[3],
                           const float* const* viewProjPerEye, uint32_t eyeCount, XrRect2Df* out)
{
    if (out == nullptr) {
        return false;
    }

    auto wholeCanvas = [&]() {
        out->offset.x = 0.0f;
        out->offset.y = 0.0f;
        out->extent.width = 1.0f;
        out->extent.height = 1.0f;
    };

    if (aabbMin == nullptr || aabbMax == nullptr || viewProjPerEye == nullptr || eyeCount == 0) {
        wholeCanvas();
        return false;
    }

    const float corners[8][3] = {
        {aabbMin[0], aabbMin[1], aabbMin[2]}, {aabbMax[0], aabbMin[1], aabbMin[2]},
        {aabbMin[0], aabbMax[1], aabbMin[2]}, {aabbMax[0], aabbMax[1], aabbMin[2]},
        {aabbMin[0], aabbMin[1], aabbMax[2]}, {aabbMax[0], aabbMin[1], aabbMax[2]},
        {aabbMin[0], aabbMax[1], aabbMax[2]}, {aabbMax[0], aabbMax[1], aabbMax[2]},
    };

    float u0 = 1.0f, v0 = 1.0f, u1 = 0.0f, v1 = 0.0f;

    for (uint32_t eye = 0; eye < eyeCount; ++eye) {
        const float* m = viewProjPerEye[eye];
        if (m == nullptr) {
            wholeCanvas();
            return false;
        }
        for (const auto& c : corners) {
            const float x = c[0], y = c[1], z = c[2];
            // Column-major 4x4 * (x, y, z, 1).
            const float rx = m[0] * x + m[4] * y + m[8] * z + m[12];
            const float ry = m[1] * x + m[5] * y + m[9] * z + m[13];
            const float rw = m[3] * x + m[7] * y + m[11] * z + m[15];

            // rw <= 0 (behind the eye) and NaN both fail this comparison.
            if (!(rw > 0.0f)) {
                wholeCanvas();
                return false;
            }

            const float ndcX = rx / rw;
            const float ndcY = ry / rw;
            const float u = (ndcX + 1.0f) * 0.5f;
            const float v = (1.0f - ndcY) * 0.5f;

            u0 = detail::MinF(u0, u);
            v0 = detail::MinF(v0, v);
            u1 = detail::MaxF(u1, u);
            v1 = detail::MaxF(v1, v);
        }
    }

    // Clamp the union to the canvas — a content AABB may exceed the frustum.
    u0 = detail::Clamp01F(u0);
    v0 = detail::Clamp01F(v0);
    u1 = detail::Clamp01F(u1);
    v1 = detail::Clamp01F(v1);

    out->offset.x = u0;
    out->offset.y = v0;
    out->extent.width = detail::MaxF(u1 - u0, 0.0f);
    out->extent.height = detail::MaxF(v1 - v0, 0.0f);
    return true;
}

/*!
 * Rebase a rect normalised to a sub-canvas (a 3D display zone) into
 * window-client-normalised space.
 *
 * `ProjectAabbToCanvasBounds` run against a zone's own view-proj yields a
 * rect normalised to that ZONE, not the app window. `XrContentBoundsDXR::bounds`
 * wants window-client-normalised space (the frame of the display processor's
 * background preview) — chaining the zone-normalised rect unchanged makes the
 * runtime's analysis region scale onto the whole window and reach into
 * whatever sits outside the zone (e.g. a Local2D speech bubble stacked above
 * a 3D avatar zone). This maps `zoneNormalised` through `zoneRectPx` (the
 * zone's own rect in window client pixels) into window-normalised space.
 *
 * The input is clamped to [0,1] in ZONE space FIRST — animation bounds
 * routinely project outside their own frustum (see the clamping test in
 * `ProjectAabbToCanvasBounds` above) — before being mapped into window space,
 * and the mapped result is clamped to [0,1] again on the way out.
 *
 * @param zoneNormalised Rect normalised to the zone's own canvas, as returned
 *                        by `ProjectAabbToCanvasBounds` for that zone's views.
 * @param zoneRectPx      The zone's rect in window CLIENT pixels (origin
 *                        top-left, y down) — what the app chained in
 *                        `XrDisplayZoneDXR`, or read back in
 *                        `XrViewDisplayRawDXR::canvasRectPx`.
 * @param windowW         Window client width in pixels.
 * @param windowH         Window client height in pixels.
 * @param out             Output rect. Always written (whole window on failure).
 * @return true on success, false on degenerate input (null `out`, zero window
 *         dimensions, or a zero/negative-area `zoneRectPx`) — `*out` is the
 *         whole window ({0,0,1,1}) on any false return except null `out`.
 */
inline bool
RebaseZoneBoundsToWindow(const XrRect2Df& zoneNormalised, const XrRect2Di& zoneRectPx,
                         uint32_t windowW, uint32_t windowH, XrRect2Df* out)
{
    if (out == nullptr) {
        return false;
    }

    auto wholeWindow = [&]() {
        out->offset.x = 0.0f;
        out->offset.y = 0.0f;
        out->extent.width = 1.0f;
        out->extent.height = 1.0f;
    };

    if (windowW == 0 || windowH == 0 || zoneRectPx.extent.width <= 0 || zoneRectPx.extent.height <= 0) {
        wholeWindow();
        return false;
    }

    // Clamp the input to [0,1] in ZONE space first — animation bounds
    // routinely project outside their own frustum. Clamp both edges
    // independently, then re-order so the rect never inverts.
    float zx0 = detail::Clamp01F(zoneNormalised.offset.x);
    float zy0 = detail::Clamp01F(zoneNormalised.offset.y);
    float zx1 = detail::Clamp01F(zoneNormalised.offset.x + zoneNormalised.extent.width);
    float zy1 = detail::Clamp01F(zoneNormalised.offset.y + zoneNormalised.extent.height);
    zx1 = detail::MaxF(zx1, zx0);
    zy1 = detail::MaxF(zy1, zy0);

    const float windowWf = (float)windowW;
    const float windowHf = (float)windowH;
    const float zoneOffXf = (float)zoneRectPx.offset.x;
    const float zoneOffYf = (float)zoneRectPx.offset.y;
    const float zoneWf = (float)zoneRectPx.extent.width;
    const float zoneHf = (float)zoneRectPx.extent.height;

    // Zone-normalised -> zone pixels -> window-normalised.
    const float wx0 = (zoneOffXf + zx0 * zoneWf) / windowWf;
    const float wx1 = (zoneOffXf + zx1 * zoneWf) / windowWf;
    const float wy0 = (zoneOffYf + zy0 * zoneHf) / windowHf;
    const float wy1 = (zoneOffYf + zy1 * zoneHf) / windowHf;

    const float ox0 = detail::Clamp01F(wx0);
    const float ox1 = detail::Clamp01F(wx1);
    const float oy0 = detail::Clamp01F(wy0);
    const float oy1 = detail::Clamp01F(wy1);

    out->offset.x = ox0;
    out->offset.y = oy0;
    out->extent.width = detail::MaxF(ox1 - ox0, 0.0f);
    out->extent.height = detail::MaxF(oy1 - oy0, 0.0f);
    return true;
}

/*!
 * Convenience: `ProjectAabbToCanvasBounds` + `RebaseZoneBoundsToWindow` in one
 * call. `aabbMin`/`aabbMax`/`viewProjPerEye`/`eyeCount` are as for
 * `ProjectAabbToCanvasBounds` (pass the zone's own per-eye view-proj
 * matrices). If `zoneRectPx.extent` is zero/negative (width or height <= 0)
 * the zone IS the whole window — no rebase is performed and the projected
 * rect is returned as-is.
 *
 * @return true on success. Returns false on the same conditions as the two
 *         steps it composes (projection failure, or a degenerate rebase
 *         input) — `*out` is the whole window ({0,0,1,1}) on any false
 *         return.
 */
inline bool
ProjectAabbToWindowBounds(const float aabbMin[3], const float aabbMax[3],
                          const float* const* viewProjPerEye, uint32_t eyeCount,
                          const XrRect2Di& zoneRectPx, uint32_t windowW, uint32_t windowH,
                          XrRect2Df* out)
{
    if (out == nullptr) {
        return false;
    }

    XrRect2Df zoneBounds{};
    if (!ProjectAabbToCanvasBounds(aabbMin, aabbMax, viewProjPerEye, eyeCount, &zoneBounds)) {
        // Projection failed closed to the whole (zone) canvas; on failure
        // the contract is "whole window", not a rebase of that fallback.
        out->offset.x = 0.0f;
        out->offset.y = 0.0f;
        out->extent.width = 1.0f;
        out->extent.height = 1.0f;
        return false;
    }

    if (zoneRectPx.extent.width <= 0 || zoneRectPx.extent.height <= 0) {
        // No zone geometry given: the zone IS the whole window, no rebase.
        *out = zoneBounds;
        return true;
    }

    return RebaseZoneBoundsToWindow(zoneBounds, zoneRectPx, windowW, windowH, out);
}

/*!
 * Zero-init `out`, stamp `XR_TYPE_CONTENT_BOUNDS_DXR`, copy `bounds` +
 * `marginNormalized`, and link it onto `fei.next`, preserving whatever chain
 * `XrFrameEndInfo` already carries. Call once per frame before `xrEndFrame`.
 *
 * There is no failure path with reference parameters (mirrors
 * `ChainRearDepthBudget` in `clip_policy.h`) — the bool return exists for
 * symmetry with this library's other Init()-style helpers and is reserved for
 * future use.
 */
inline bool
ChainContentBounds(XrFrameEndInfo& fei, XrContentBoundsDXR& out, const XrRect2Df& bounds,
                    float marginNormalized = 0.0f)
{
    out = XrContentBoundsDXR{};
    out.type = (XrStructureType)XR_TYPE_CONTENT_BOUNDS_DXR;
    out.next = fei.next;
    out.bounds = bounds;
    out.marginNormalized = marginNormalized;
    fei.next = &out;
    return true;
}

} // namespace dxr

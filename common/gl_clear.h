// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Display-referred clears for OpenGL (runtime #1647).
 *
 * Header-only, and it pulls in NO GL header and links NO GL. The rule and the
 * reasoning live in `clear_policy.h`.
 *
 * ── Why GL is the awkward one ───────────────────────────────────────────────
 *
 * On every other API the target's format settles whether a write encodes. On
 * GL it does not. Encode-on-write is two conditions ANDed, and both are STATE:
 *
 *   1. `GL_FRAMEBUFFER_SRGB` is enabled, and
 *   2. the draw framebuffer attachment's
 *      `GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING` is `GL_SRGB`.
 *
 * A `GL_SRGB8_ALPHA8` attachment with the toggle OFF encodes nothing. The
 * DisplayXR GL apps are on an `_SRGB` swapchain today and are un-regressed
 * for exactly that reason — none of them enables it. Mind GL's inverted
 * vocabulary too: `GL_LINEAR` for that query means "no encoding is applied",
 * not "the content is linear".
 *
 * ── Why the caller passes the entry points ─────────────────────────────────
 *
 * `glGetFramebufferAttachmentParameteriv` is GL 3.0. On Windows `opengl32.dll`
 * exports only GL 1.1, so it has to come through `wglGetProcAddress`; on macOS
 * it comes from the framework and on Linux from `libGL`. How it is resolved is
 * the consumer's business, and displayxr-common deliberately links no GL
 * loader. So the query takes the function pointers, declared structurally here
 * so this header needs no GL header at all.
 *
 * @code
 *   dxr::GlDrawBufferState st = dxr::GlQueryDrawBufferState(
 *       glIsEnabled, glGetFramebufferAttachmentParameteriv, glGetError);
 *   float c[4];
 *   dxr::GlDisplayReferredClearColor(st, kBackground, c);
 *   glClearColor(c[0], c[1], c[2], c[3]);
 *   glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
 * @endcode
 *
 * Query once per framebuffer configuration and cache it; do not query per
 * frame (three round-trips to the driver for a value that does not move).
 *
 * ── What is NOT settled here, and must be checked on a real driver ─────────
 *
 * Whether a CLEAR honours `GL_FRAMEBUFFER_SRGB` at all. The core spec applies
 * the conversion to clears as well as to fragment writes, but drivers have
 * historically diverged and GLES / ANGLE differ again. No host test can settle
 * it. If a driver turns out not to convert clears, the fix is at the call site
 * (pass `ClearValueSpace::DisplayReferred`), not in this rule.
 */

#pragma once

#include "clear_policy.h"

//! GL entry points are `__stdcall` on Windows (APIENTRY). Irrelevant on x64,
//! wrong on 32-bit, so spell it out rather than rely on the target.
#ifdef _WIN32
#define DXR_GLAPIENTRY __stdcall
#else
#define DXR_GLAPIENTRY
#endif

namespace dxr {

// GL enum values, verified against gl3.h. Spelled out so this header needs no
// GL header on any platform.
inline constexpr unsigned int kGlDrawFramebuffer = 0x8CA9;
inline constexpr unsigned int kGlFramebufferAttachmentColorEncoding = 0x8210;
inline constexpr unsigned int kGlColorAttachment0 = 0x8CE0;
inline constexpr unsigned int kGlFramebufferSrgbCap = 0x8DB9;  // GL_FRAMEBUFFER_SRGB
inline constexpr unsigned int kGlNoError = 0;

using GlIsEnabledFn = unsigned char(DXR_GLAPIENTRY *)(unsigned int);
using GlGetFramebufferAttachmentParameterivFn =
    void(DXR_GLAPIENTRY *)(unsigned int, unsigned int, unsigned int, int *);
using GlGetErrorFn = unsigned int(DXR_GLAPIENTRY *)();

/*!
 * Query the current GL draw framebuffer's encode state.
 *
 * Requires a CURRENT GL context — this issues GL calls and must never be
 * invoked without one. Any null entry point, or a query that raises a GL
 * error, yields a state that `GlClearValueSpace()` reports as `Unknown`; that
 * is the honest answer on GLES 2 / pre-3.0 desktop GL without
 * `EXT_sRGB_write_control`, where the toggle does not exist.
 *
 * `attachment` is `GL_COLOR_ATTACHMENT0` for an FBO (the DisplayXR case: GL
 * apps render into an XR swapchain FBO) or `GL_BACK_LEFT` / `GL_BACK` for the
 * default framebuffer. The default framebuffer's reported encoding is
 * driver-dependent for a window-system-supplied buffer, which is a further
 * reason to prefer the FBO path.
 */
inline GlDrawBufferState
GlQueryDrawBufferState(GlIsEnabledFn isEnabled,
                       GlGetFramebufferAttachmentParameterivFn getAttachmentParameteriv,
                       GlGetErrorFn getError = nullptr,
                       unsigned int attachment = kGlColorAttachment0)
{
    GlDrawBufferState st;
    if (isEnabled == nullptr || getAttachmentParameteriv == nullptr) {
        return st;  // srgbQueryable stays false -> Unknown
    }

    // Drain any error left by earlier caller code, so the check below can only
    // report an error THIS query raised.
    if (getError != nullptr) {
        for (int i = 0; i < 16 && getError() != kGlNoError; i++) {
        }
    }

    st.framebufferSrgbEnabled = isEnabled(kGlFramebufferSrgbCap) != 0;

    int encoding = 0;
    getAttachmentParameteriv(kGlDrawFramebuffer, attachment,
                             kGlFramebufferAttachmentColorEncoding, &encoding);

    if (getError != nullptr && getError() != kGlNoError) {
        return GlDrawBufferState{};  // the query failed; say Unknown, do not guess
    }

    st.colorEncoding = encoding;
    st.srgbQueryable = true;
    return st;
}

//! Convert a display-referred clear for the queried GL draw buffer. `outRGBA`
//! is always filled; feed it straight to `glClearColor`.
inline ClearValueSpace
GlDisplayReferredClearColor(const GlDrawBufferState &st,
                            const float displayReferredRGBA[4],
                            float outRGBA[4])
{
    const ClearValueSpace space = GlClearValueSpace(st);
    if (space == ClearValueSpace::Unknown) {
        ReportUnknownClearTarget("OpenGL", (long long)st.colorEncoding);
    }
    return ApplyClearValueSpace(space, displayReferredRGBA, outRGBA);
}

//! @overload For a caller that already knows the space (a blit/resolve
//! pipeline, or a driver worked around at the call site).
inline ClearValueSpace
GlDisplayReferredClearColor(ClearValueSpace space,
                            const float displayReferredRGBA[4],
                            float outRGBA[4])
{
    return ApplyClearValueSpace(space, displayReferredRGBA, outRGBA);
}

} // namespace dxr

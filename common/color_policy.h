// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Color-swapchain format policy — the one place that decides whether an
 *         app asks for an `_SRGB` swapchain and therefore whether its shaders
 *         must emit scene-linear values.
 *
 * ADR-021 / runtime #1589. Two facts, one rule:
 *
 *  - An **honest `_SRGB`** color swapchain is correct on BOTH the runtime that
 *    ships today (which passes the app's bytes through: the app's own `_SRGB`
 *    render target does the encode, so the runtime receives encoded bytes) and
 *    on the format-honest runtime that is coming (which decodes an `_SRGB`
 *    swapchain on read and re-encodes on write — an identity round-trip).
 *    That is what makes migrating apps to `_SRGB` **first** safe.
 *  - A **UNORM** color swapchain is the ambiguous one: today it means "whatever
 *    bytes the app wrote"; after the flip it means "linear". An app that writes
 *    display-referred bytes into UNORM washes out after the flip.
 *
 * So: prefer `_SRGB` by default, and when the swapchain IS `_SRGB`, the app's
 * pixel shaders must emit **scene-linear** so the render target's hardware
 * encode reproduces exactly the bytes the app authored. Those two decisions are
 * one decision, which is why they live in one file.
 *
 * Deliberately free of OpenXR, D3D and Win32 types so it compiles on every
 * platform and is unit-testable without a device (tests/common_smoke.cpp).
 * It also does no logging — the caller owns the log line, because only the
 * caller knows which swapchain it is creating.
 */

#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace dxr {

//! What `DXR_SWAPCHAIN_ENCODING` asked for.
enum class ColorEncodingPreference {
    HonestSrgb,  //!< default (env unset or unrecognized) — prefer an `_SRGB` format
    ForceSrgb,   //!< `DXR_SWAPCHAIN_ENCODING=srgb`
    ForceUnorm,  //!< `DXR_SWAPCHAIN_ENCODING=unorm` — the A/B escape hatch
};

//! Why `ChooseColorSwapchainFormat()` returned what it returned.
struct ColorFormatChoice {
    int64_t format = 0;                //!< the format to pass to xrCreateSwapchain (0 = none advertised)
    ColorEncodingPreference preference = ColorEncodingPreference::HonestSrgb;
    bool isSrgb = false;               //!< the chosen format is a known `_SRGB` code
    bool fellBack = false;             //!< the preference could not be satisfied; `formats[0]` was used
    bool envUnrecognized = false;      //!< `DXR_SWAPCHAIN_ENCODING` was set to something other than srgb|unorm
};

/*!
 * Parse `DXR_SWAPCHAIN_ENCODING`. `value` may be nullptr (unset) or empty.
 * Anything other than `srgb` / `unorm` (case-insensitive) is HonestSrgb, and
 * the caller is expected to warn — see `ColorFormatChoice::envUnrecognized`.
 */
ColorEncodingPreference
ColorEncodingPreferenceFromEnv(const char *value);

//! Read `DXR_SWAPCHAIN_ENCODING` from the environment.
ColorEncodingPreference
ColorEncodingPreferenceFromEnvironment();

/*!
 * Is `format` a known `_SRGB` color code (DXGI / Vulkan / GL / Metal)?
 *
 * Also THE rule for a Vulkan app's own internal color target when the frame
 * ends in a `vkCmdBlitImage` into the swapchain image rather than a direct
 * render into it. `vkCmdBlitImage` converts through the two IMAGES' formats,
 * so the pair has to agree:
 *
 *     internalFormat = dxr::IsSrgbColorFormat(swapchainFormat)
 *                          ? <your _SRGB format>   // e.g. VK_FORMAT_R8G8B8A8_SRGB
 *                          : <your UNORM format>;  // e.g. VK_FORMAT_R8G8B8A8_UNORM
 *
 * `_SRGB`→`_SRGB` decodes then re-encodes (identity) and UNORM→UNORM copies the
 * bytes; the two MIXED pairs are half-conversions — display-referred UNORM into
 * `_SRGB` encodes a second time (washed out), `_SRGB` into UNORM decodes and
 * never re-encodes (too dark). See README § *If you blit into the swapchain
 * (Vulkan)*.
 */
bool
IsSrgbColorFormat(int64_t format);

//! Is `format` a known plain-UNORM color code (DXGI / Vulkan / GL / Metal)?
bool
IsUnormColorFormat(int64_t format);

/*!
 * The `_SRGB` sibling of a UNORM code — same channel order, same bit layout.
 * Returns 0 when there is no known sibling. Channel order matters (a BGRA
 * runtime must stay BGRA), so the sibling is always preferred over "some other
 * advertised sRGB format".
 */
int64_t
SrgbSiblingOf(int64_t unormFormat);

//! The plain-UNORM sibling of an `_SRGB` code; 0 when unknown.
int64_t
UnormSiblingOf(int64_t srgbFormat);

/*!
 * THE selection rule. `formats` is `xrEnumerateSwapchainFormats`' list in the
 * runtime's own preference order.
 *
 *  - ForceUnorm  → the first advertised known UNORM code; else `formats[0]` (fellBack).
 *  - ForceSrgb   → as HonestSrgb, but a miss is reported as fellBack.
 *  - HonestSrgb (default) →
 *      1. `formats[0]` if it is already `_SRGB`;
 *      2. else the advertised `_SRGB` **sibling** of `formats[0]` (keeps channel order);
 *      3. else the first advertised known `_SRGB` code, in the runtime's order;
 *      4. else `formats[0]` with fellBack = true — no `_SRGB` format exists, so
 *         the app keeps today's bytes and the caller should say so once.
 *
 * Pure: reads no environment, touches no global state.
 */
ColorFormatChoice
ChooseColorSwapchainFormat(const std::vector<int64_t> &formats, ColorEncodingPreference pref);

/*!
 * Record the color format the app actually created its main color swapchain
 * with. First non-zero call wins — the projection swapchain is the authority;
 * later quad / zone / Local2D swapchains inherit the same encoding.
 *
 * This is what closes the ordering gap: D3D11 shaders are compiled when the
 * device is created, which is BEFORE `xrCreateSession`, so the renderer cannot
 * ask the runtime what it chose. It compiles both variants and asks here.
 */
void
NoteColorSwapchainFormat(int64_t format);

//! Was the noted color swapchain format an `_SRGB` one? False before any note.
bool
ColorSwapchainIsSrgb();

/*!
 * Must the app's pixel shaders emit SCENE-LINEAR values?
 *
 *  - `DXR_TRUE_LINEAR` unset → follows `ColorSwapchainIsSrgb()`. This is the
 *    automatic, correct-by-construction case: an `_SRGB` render target encodes
 *    on write, so the shader must hand it linear values for the stored bytes to
 *    equal the authored (display-referred) colors.
 *  - `DXR_TRUE_LINEAR=0` / `false` / `off` → forced OFF (write authored bytes raw).
 *  - `DXR_TRUE_LINEAR` = anything else → forced ON. With
 *    `DXR_SWAPCHAIN_ENCODING=unorm` this is the ADR-021 matrix's
 *    **true-linear-into-UNORM** cell: linear radiance in a UNORM swapchain.
 *
 * Re-read on every call (cheap) rather than cached, because the swapchain
 * format is noted after the shaders are compiled.
 */
bool
RenderSceneLinear();

/*!
 * The two transfer curves are `inline` IN THE HEADER on purpose: they are the
 * only part of this file a header-only consumer needs, and `displayxr::rules`
 * (the INTERFACE target the Android legs consume — `displayxr::common` is
 * STATIC and carries Win32/AppKit sources, so it cannot be linked there at
 * all) can offer them only if there is no `.cpp` to link. `clear_policy.h`
 * depends on that. Same math, same call sites, no behaviour change.
 */

//! Standard sRGB EOTF: display-referred [0,1] → scene-linear [0,1].
inline float
DisplayReferredToSceneLinear(float c)
{
    if (c <= 0.04045f) {
        return c / 12.92f;
    }
    return std::pow((c + 0.055f) / 1.055f, 2.4f);
}

//! Standard sRGB OETF: scene-linear [0,1] → display-referred [0,1].
inline float
SceneLinearToDisplayReferred(float c)
{
    if (c <= 0.0031308f) {
        return c * 12.92f;
    }
    return 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

} // namespace dxr

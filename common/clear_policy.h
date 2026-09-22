// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  What space a CLEAR value handed to a given render target has to be
 *         in — the per-API, per-target rule behind the display-referred clear
 *         helpers (runtime #1647).
 *
 * THE RULE, ONCE, BECAUSE EVERY BUG HERE IS A MISREADING OF IT:
 *
 *   The question is **what space the content written into this target is in**,
 *   and the target's format answers it **only when that target is the thing
 *   that encodes**. Where a later blit or resolve does the encoding, the
 *   caller must say.
 *
 * Both halves are load-bearing.
 *
 *   - A clear value is taken in the attachment's OWN space on every API
 *     (`ClearRenderTargetView`, `VkClearColorValue`, `pClearValues`,
 *     `glClearColor`). So an `_SRGB` attachment ENCODES the clear on write,
 *     and a display-referred background written raw comes out brighter — the
 *     navy `13,13,64` measured as `63,63,137` on a panel, which is what this
 *     file exists to stop.
 *   - But an app that renders into an internal UNORM image and ends the frame
 *     with `vkCmdBlitImage` into an `_SRGB` swapchain has an attachment whose
 *     FORMAT says "stores verbatim" while its CONTENT is scene-linear (its
 *     shaders emit linear precisely because the blit will encode). Reading
 *     only the format there leaves the clear un-converted and the bug intact.
 *     Two shipping consumers are built exactly that way.
 *
 * Hence two entry points on every wrapper: one that derives the space from the
 * target's format, and one that takes `ClearValueSpace` because the caller
 * knows something the format cannot express. Prefer the first; the second is
 * not an escape hatch, it is the correct call for a blit/resolve pipeline.
 *
 * THE PREDICATES ARE API-SCOPED, AND THAT IS NOT PEDANTRY.
 * `color_policy.h`'s `IsSrgbColorFormat()` is a UNION of every API's codes. It
 * is safe where it is used — a session enumerates exactly one API's codes — and
 * UNSAFE here, because a clear helper is handed an arbitrary internal target
 * format. The codes genuinely collide (verified against the real headers):
 *
 *     code | IsSrgbColorFormat() | actual VkFormat            | if reused here
 *     -----+---------------------+----------------------------+----------------
 *       91 | sRGB (DXGI BGRA8)   | VK_FORMAT_R16G16B16A16_UNORM | silently DARK
 *       71 | sRGB (Metal RGBA8)  | VK_FORMAT_R16_SNORM          | silently DARK
 *       81 | sRGB (Metal BGRA8)  | VK_FORMAT_R16G16_UINT        | silently DARK
 *       87 | UNORM (DXGI BGRA8)  | VK_FORMAT_R16G16B16_SSCALED  | misclassified
 *       28 | UNORM (DXGI RGBA8)  | VK_FORMAT_R8G8B8_SINT        | misclassified
 *
 * So never call `IsSrgbColorFormat()` on a target format. Call the predicate
 * for the API you are actually holding. `tests/common_smoke.cpp` asserts the
 * cross-API negatives directly.
 *
 * WHAT THE TABLES COVER. The 8-bit UNORM/`_SRGB` families only — the formats
 * where "does a write apply a transfer function" has one answer. A 16-bit,
 * 10-bit or float attachment applies no transfer function either, but what its
 * CONTENT is in is the caller's decision (an HDR float target usually carries
 * scene-linear), so the tables decline to guess and return `Unknown` rather
 * than quietly answering `DisplayReferred`. State it with the explicit
 * overload. A `_TYPELESS` DXGI code is `Unknown` for the same reason and a
 * sharper one: a typeless resource's encode is armed by the VIEW, so the
 * resource format cannot answer at all.
 *
 * Header-only, no graphics headers, no logging, no globals — so it is reachable
 * from `displayxr::rules` (the Android legs) as well as `displayxr::common`,
 * and every line of it is unit-testable with no device.
 */

#pragma once

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <utility>

#include "color_policy.h"

namespace dxr {

/*!
 * What space a clear value handed to a particular render target must be in.
 *
 * Descriptive rather than a bool on purpose (the shape runtime PR #1669
 * settled on): a bool cannot express "I could not tell", and that third state
 * is precisely where the colour bugs live.
 */
enum class ClearValueSpace {
    //! Stored verbatim — pass the value exactly as authored. A plain UNORM
    //! target holding display-referred content.
    DisplayReferred,
    //! Encoded before it lands — pass scene-linear. Either an `_SRGB` target
    //! (the hardware applies the OETF on write) or a verbatim target whose
    //! content is scene-linear because a later `_SRGB` blit/resolve encodes it.
    SceneLinear,
    //! The code is not one this API's table classifies. Callers must NOT guess
    //! — see `ApplyClearValueSpace()`.
    Unknown,
};

//! Short name for a log line.
inline const char *
ClearValueSpaceName(ClearValueSpace s)
{
    switch (s) {
    case ClearValueSpace::DisplayReferred: return "display-referred";
    case ClearValueSpace::SceneLinear: return "scene-linear";
    default: return "unknown";
    }
}

// ── Vulkan ───────────────────────────────────────────────────────────────────
//
// Values verified against vulkan_core.h. The 8-bit family only; everything else
// (16-bit, 10-bit, float, depth, block-compressed) is Unknown by design.

//! Does a write into a target of this `VkFormat` encode, or store verbatim?
inline constexpr ClearValueSpace
VulkanClearValueSpace(int64_t vkFormat)
{
    switch (vkFormat) {
    case 15:  // VK_FORMAT_R8_SRGB
    case 22:  // VK_FORMAT_R8G8_SRGB
    case 29:  // VK_FORMAT_R8G8B8_SRGB
    case 36:  // VK_FORMAT_B8G8R8_SRGB
    case 43:  // VK_FORMAT_R8G8B8A8_SRGB
    case 50:  // VK_FORMAT_B8G8R8A8_SRGB
    case 57:  // VK_FORMAT_A8B8G8R8_SRGB_PACK32
        return ClearValueSpace::SceneLinear;
    case 9:   // VK_FORMAT_R8_UNORM
    case 16:  // VK_FORMAT_R8G8_UNORM
    case 23:  // VK_FORMAT_R8G8B8_UNORM
    case 30:  // VK_FORMAT_B8G8R8_UNORM
    case 37:  // VK_FORMAT_R8G8B8A8_UNORM
    case 44:  // VK_FORMAT_B8G8R8A8_UNORM
    case 51:  // VK_FORMAT_A8B8G8R8_UNORM_PACK32
        return ClearValueSpace::DisplayReferred;
    default:
        return ClearValueSpace::Unknown;
    }
}

// ── Direct3D (DXGI) ──────────────────────────────────────────────────────────
//
// Values verified against dxgiformat.h. Shared by D3D11 and D3D12 — the format
// enum is the same; only how the caller obtains it differs (D3D11 can ask the
// RTV with GetDesc(); a D3D12 descriptor handle is an opaque address and
// carries no format, so a D3D12 caller must pass the one it created the RTV
// with).

//! Does a write into a target of this `DXGI_FORMAT` encode, or store verbatim?
inline constexpr ClearValueSpace
DxgiClearValueSpace(int64_t dxgiFormat)
{
    switch (dxgiFormat) {
    case 29:  // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
    case 91:  // DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
    case 93:  // DXGI_FORMAT_B8G8R8X8_UNORM_SRGB
        return ClearValueSpace::SceneLinear;
    case 28:  // DXGI_FORMAT_R8G8B8A8_UNORM
    case 49:  // DXGI_FORMAT_R8G8_UNORM
    case 61:  // DXGI_FORMAT_R8_UNORM
    case 65:  // DXGI_FORMAT_A8_UNORM
    case 87:  // DXGI_FORMAT_B8G8R8A8_UNORM
    case 88:  // DXGI_FORMAT_B8G8R8X8_UNORM
        return ClearValueSpace::DisplayReferred;
    // 27 / 90 / 92 = the _TYPELESS siblings. Deliberately Unknown: the encode
    // is armed by the VIEW, so the resource format answers nothing. The runtime
    // hands out TYPELESS swapchain textures, which is exactly why every DXGI
    // caller must pass its RTV's format rather than the texture's.
    default:
        return ClearValueSpace::Unknown;
    }
}

// ── Metal ────────────────────────────────────────────────────────────────────
//
// Values verified against MTLPixelFormat.h. Predicate ONLY — there is
// deliberately no Metal wrapper translation unit, because displayxr-common
// carries no Metal code at all and every Metal consumer in the org pins
// MTLPixelFormatBGRA8Unorm and never enumerates for `_SRGB`. The table costs
// four rows and keeps the knowledge for whoever does enumerate one day.

//! Does a write into a target of this `MTLPixelFormat` encode, or store verbatim?
inline constexpr ClearValueSpace
MetalClearValueSpace(int64_t mtlPixelFormat)
{
    switch (mtlPixelFormat) {
    case 11:  // MTLPixelFormatR8Unorm_sRGB
    case 31:  // MTLPixelFormatRG8Unorm_sRGB
    case 71:  // MTLPixelFormatRGBA8Unorm_sRGB
    case 81:  // MTLPixelFormatBGRA8Unorm_sRGB
        return ClearValueSpace::SceneLinear;
    case 10:  // MTLPixelFormatR8Unorm
    case 30:  // MTLPixelFormatRG8Unorm
    case 70:  // MTLPixelFormatRGBA8Unorm
    case 80:  // MTLPixelFormatBGRA8Unorm
        return ClearValueSpace::DisplayReferred;
    default:
        return ClearValueSpace::Unknown;
    }
}

// ── OpenGL ───────────────────────────────────────────────────────────────────
//
// GL is the one API where the format alone is NOT the answer. Encode-on-write
// is two conditions ANDed, and both are STATE:
//
//   1. GL_FRAMEBUFFER_SRGB is enabled (the write-control toggle), and
//   2. the draw framebuffer attachment's
//      GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING is GL_SRGB.
//
// An GL_SRGB8_ALPHA8 attachment with the toggle OFF encodes nothing. That is
// not a hypothetical: the DisplayXR GL test apps are on an `_SRGB` swapchain
// today and are un-regressed precisely because none of them enables it (there
// is a live `// TODO: ... we may need glEnable(GL_FRAMEBUFFER_SRGB)` in
// cube_handle_gl_win). The day that TODO is acted on, the clears move.
//
// Mind GL's inverted vocabulary: GL_LINEAR here means "NO encoding is applied"
// (the UNORM case), not "the content is linear".

inline constexpr int kGlSrgb = 0x8C40;    //!< GL_SRGB
inline constexpr int kGlLinear = 0x2601;  //!< GL_LINEAR ("no encoding applied")
//! Pass when the query could not be made at all — no current context, no GL 3.0
//! entry point, or a GL error. Never pass 0 meaning "false".
inline constexpr int kGlColorEncodingUnknown = 0;

//! The two queried facts about the current GL draw framebuffer.
struct GlDrawBufferState {
    //! GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING for the attachment being
    //! cleared, or kGlColorEncodingUnknown.
    int colorEncoding = kGlColorEncodingUnknown;
    //! glIsEnabled(GL_FRAMEBUFFER_SRGB). Meaningless unless `srgbQueryable`.
    bool framebufferSrgbEnabled = false;
    //! False on GLES 2 / pre-3.0 desktop GL without EXT_sRGB_write_control,
    //! where the toggle does not exist and the encoding is unqueryable.
    bool srgbQueryable = false;
};

//! Apply the GL rule to already-queried state. Pure — this is the testable part.
inline constexpr ClearValueSpace
GlClearValueSpace(const GlDrawBufferState &st)
{
    if (!st.srgbQueryable || st.colorEncoding == kGlColorEncodingUnknown) {
        return ClearValueSpace::Unknown;
    }
    if (st.colorEncoding == kGlSrgb) {
        return st.framebufferSrgbEnabled ? ClearValueSpace::SceneLinear
                                         : ClearValueSpace::DisplayReferred;
    }
    if (st.colorEncoding == kGlLinear) {
        // No encoding applied whatever the toggle says.
        return ClearValueSpace::DisplayReferred;
    }
    return ClearValueSpace::Unknown;
}

// ── Applying it ──────────────────────────────────────────────────────────────

/*!
 * Convert a display-referred clear into the space `space` requires.
 *
 * `outRGBA` is ALWAYS filled (it may alias `inRGBA`). Alpha is linear in both
 * spaces and is never converted. Returns the space actually applied, so a
 * caller cannot read `Unknown` as `false` by accident.
 *
 * ON `Unknown`: the value is copied through UNCONVERTED, deliberately.
 *
 *   - Not converting reproduces today's shipped bytes exactly, so an
 *     unrecognised format can never be a REGRESSION introduced by this helper
 *     — only a pre-existing miss, which the caller's one-shot warning names.
 *     That property is worth more than being right slightly more often.
 *   - Converting on a guess produces DARKENING, which is the harder failure to
 *     spot: a too-dark background reads as plausible where a washed-out one
 *     does not.
 *   - Refusing (assert / abort / an un-cleared frame) turns a colour nit into a
 *     crash in shipped software. Wrong trade.
 *   - It is the same instinct as `ChooseColorSwapchainFormat()`'s `fellBack`
 *     path, which is documented as "the app keeps writing display-referred
 *     bytes".
 *
 * It is not silent: pair it with `ReportUnknownClearTarget()`.
 */
inline ClearValueSpace
ApplyClearValueSpace(ClearValueSpace space, const float inRGBA[4], float outRGBA[4])
{
    const bool convert = (space == ClearValueSpace::SceneLinear);
    for (int i = 0; i < 3; i++) {
        outRGBA[i] = convert ? DisplayReferredToSceneLinear(inRGBA[i]) : inRGBA[i];
    }
    outRGBA[3] = inRGBA[3];  // alpha is linear in both spaces
    return space;
}

/*!
 * One-shot, de-duplicated diagnostic for a target format this file cannot
 * classify. Writes ONE line to stderr per distinct (api, code) pair, ever.
 *
 * De-duplication is not politeness: a clear runs once per view per frame, so an
 * un-deduplicated line is exactly the per-frame log bloat the project bans.
 * stderr rather than `logging.h` because that header is Windows-only and this
 * one must compile everywhere.
 *
 * Returns true the first time a given pair is seen.
 */
inline bool
ReportUnknownClearTarget(const char *api, long long formatCode)
{
    static std::mutex mu;
    static std::set<std::pair<std::string, long long>> seen;
    const std::pair<std::string, long long> key(api != nullptr ? api : "?", formatCode);
    {
        std::lock_guard<std::mutex> lock(mu);
        if (!seen.insert(key).second) {
            return false;
        }
    }
    std::fprintf(stderr,
                 "[dxr clear_policy] %s target format %lld (0x%llX) is not classified; "
                 "clearing with the authored display-referred value unconverted. "
                 "If this target encodes on write, pass dxr::ClearValueSpace explicitly.\n",
                 key.first.c_str(), formatCode, (unsigned long long)formatCode);
    return true;
}

} // namespace dxr

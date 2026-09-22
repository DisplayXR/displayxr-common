// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Color-swapchain format policy (see color_policy.h).
 */

#include "color_policy.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace dxr {
namespace {

//! Case-insensitive ASCII compare — no <strings.h>/_stricmp, so this TU stays
//! portable to every platform the header promises.
bool
EqualsIgnoreCaseAscii(const char *a, const char *b)
{
    if (a == nullptr || b == nullptr) {
        return false;
    }
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) {
            return false;
        }
    }
    return *a == '\0' && *b == '\0';
}

/*!
 * The UNORM ↔ `_SRGB` sibling table, by API.
 *
 *   DXGI  R8G8B8A8_UNORM        28 ↔ 29 R8G8B8A8_UNORM_SRGB
 *   DXGI  B8G8R8A8_UNORM        87 ↔ 91 B8G8R8A8_UNORM_SRGB
 *   VK    R8G8B8A8_UNORM        37 ↔ 43 R8G8B8A8_SRGB
 *   VK    B8G8R8A8_UNORM        44 ↔ 50 B8G8R8A8_SRGB
 *   VK    A8B8G8R8_UNORM_PACK32 51 ↔ 57 A8B8G8R8_SRGB_PACK32
 *   GL    RGBA8             0x8058 ↔ 0x8C43 SRGB8_ALPHA8
 *   Metal RGBA8Unorm            70 ↔ 71 RGBA8Unorm_sRGB
 *   Metal BGRA8Unorm            80 ↔ 81 BGRA8Unorm_sRGB
 *
 * Codes collide across APIs (DXGI 29 is also VK_FORMAT_R8G8B8_SRGB, Metal 70 is
 * also a VK 16-bit code, …). That is tolerable — and is what the pre-existing
 * `DXR_SWAPCHAIN_ENCODING` lists already did — because a session only ever
 * enumerates ONE API's codes, and the sibling rule additionally requires the
 * sibling to be advertised by that same runtime before it is taken.
 */
struct FormatPair {
    int64_t unorm;
    int64_t srgb;
};

const FormatPair kSiblings[] = {
    {28, 29},         // DXGI R8G8B8A8
    {87, 91},         // DXGI B8G8R8A8
    {37, 43},         // VK   R8G8B8A8
    {44, 50},         // VK   B8G8R8A8
    {51, 57},         // VK   A8B8G8R8_PACK32
    {0x8058, 0x8C43}, // GL   RGBA8 / SRGB8_ALPHA8
    {70, 71},         // Metal RGBA8Unorm
    {80, 81},         // Metal BGRA8Unorm
};

bool
Advertised(const std::vector<int64_t> &formats, int64_t f)
{
    for (int64_t x : formats) {
        if (x == f) {
            return true;
        }
    }
    return false;
}

//! Sticky record of the main color swapchain's format. 0 = not noted yet.
int64_t g_noted_color_format = 0;

} // namespace

bool
IsSrgbColorFormat(int64_t format)
{
    for (const FormatPair &p : kSiblings) {
        if (p.srgb == format) {
            return true;
        }
    }
    return false;
}

bool
IsUnormColorFormat(int64_t format)
{
    for (const FormatPair &p : kSiblings) {
        if (p.unorm == format) {
            return true;
        }
    }
    return false;
}

int64_t
SrgbSiblingOf(int64_t unormFormat)
{
    for (const FormatPair &p : kSiblings) {
        if (p.unorm == unormFormat) {
            return p.srgb;
        }
    }
    return 0;
}

int64_t
UnormSiblingOf(int64_t srgbFormat)
{
    for (const FormatPair &p : kSiblings) {
        if (p.srgb == srgbFormat) {
            return p.unorm;
        }
    }
    return 0;
}

ColorEncodingPreference
ColorEncodingPreferenceFromEnv(const char *value)
{
    if (value == nullptr || *value == '\0') {
        return ColorEncodingPreference::HonestSrgb;
    }
    if (EqualsIgnoreCaseAscii(value, "srgb")) {
        return ColorEncodingPreference::ForceSrgb;
    }
    if (EqualsIgnoreCaseAscii(value, "unorm")) {
        return ColorEncodingPreference::ForceUnorm;
    }
    return ColorEncodingPreference::HonestSrgb;
}

ColorEncodingPreference
ColorEncodingPreferenceFromEnvironment()
{
    return ColorEncodingPreferenceFromEnv(getenv("DXR_SWAPCHAIN_ENCODING"));
}

ColorFormatChoice
ChooseColorSwapchainFormat(const std::vector<int64_t> &formats, ColorEncodingPreference pref)
{
    ColorFormatChoice out;
    out.preference = pref;
    if (formats.empty()) {
        return out;
    }

    const int64_t first = formats[0];

    if (pref == ColorEncodingPreference::ForceUnorm) {
        // Honour the runtime's own channel-order preference: try formats[0]'s
        // UNORM sibling before scanning, so a BGRA runtime stays BGRA.
        int64_t want = IsUnormColorFormat(first) ? first : UnormSiblingOf(first);
        if (want != 0 && Advertised(formats, want)) {
            out.format = want;
            return out;
        }
        for (int64_t f : formats) {
            if (IsUnormColorFormat(f)) {
                out.format = f;
                return out;
            }
        }
        out.format = first;
        out.isSrgb = IsSrgbColorFormat(first);
        out.fellBack = true;
        return out;
    }

    // HonestSrgb (the default) and ForceSrgb take the same path; they differ
    // only in how loudly the caller reports a miss.
    if (IsSrgbColorFormat(first)) {
        out.format = first;
        out.isSrgb = true;
        return out;
    }
    const int64_t sibling = SrgbSiblingOf(first);
    if (sibling != 0 && Advertised(formats, sibling)) {
        out.format = sibling;
        out.isSrgb = true;
        return out;
    }
    for (int64_t f : formats) {
        if (IsSrgbColorFormat(f)) {
            out.format = f;
            out.isSrgb = true;
            return out;
        }
    }

    // No `_SRGB` format advertised at all — keep today's behaviour verbatim.
    out.format = first;
    out.isSrgb = false;
    out.fellBack = true;
    return out;
}

void
NoteColorSwapchainFormat(int64_t format)
{
    if (format != 0 && g_noted_color_format == 0) {
        g_noted_color_format = format;
    }
}

bool
ColorSwapchainIsSrgb()
{
    return IsSrgbColorFormat(g_noted_color_format);
}

bool
RenderSceneLinear()
{
    const char *e = getenv("DXR_TRUE_LINEAR");
    if (e != nullptr && *e != '\0') {
        if (EqualsIgnoreCaseAscii(e, "0") || EqualsIgnoreCaseAscii(e, "false") ||
            EqualsIgnoreCaseAscii(e, "off") || EqualsIgnoreCaseAscii(e, "no")) {
            return false;
        }
        return true;
    }
    return ColorSwapchainIsSrgb();
}

// DisplayReferredToSceneLinear / SceneLinearToDisplayReferred moved to the
// header as `inline` so header-only consumers (displayxr::rules, and through it
// clear_policy.h on the Android legs) can reach them. Same math.

} // namespace dxr

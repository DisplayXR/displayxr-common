// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief Zones-by-default: one full-window XrDisplayZoneDXR, shared plumbing.
 */

#include "zone_default.h"

#include <stdio.h>

// This file is platform-neutral, so it cannot use logging.h (Win32-only).
// These one-shot init-time lines go to stderr like the rest of the neutral
// helpers; apps with a file logger see the same facts via their own paths.
#define ZONE_LOG(...)                                                                              \
    do {                                                                                           \
        fprintf(stderr, "FullWindowZone: " __VA_ARGS__);                                           \
        fputc('\n', stderr);                                                                       \
    } while (0)

namespace dxr {

bool
FullWindowZoneInit(FullWindowZone& z, XrInstance instance, XrSession session)
{
    z.available = false;
    z.session = session;
    z.lastW = z.lastH = -1;

    if (instance == XR_NULL_HANDLE || session == XR_NULL_HANDLE) {
        return false;
    }

    // Entry points resolve only when the app enabled XR_DXR_display_zones at
    // instance creation AND the runtime implements it — so a missing pfn
    // covers both "app didn't opt in" and "old runtime" in one check.
    PFN_xrGetDisplayZoneCapabilitiesDXR pfnCaps = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(instance, "xrGetDisplayZoneCapabilitiesDXR",
                                        (PFN_xrVoidFunction*)&pfnCaps)) ||
        pfnCaps == nullptr) {
        ZONE_LOG("xrGetDisplayZoneCapabilitiesDXR not resolvable "
                 "(extension not enabled, or runtime too old) -- plain frames");
        return false;
    }
    if (XR_FAILED(xrGetInstanceProcAddr(instance, "xrGetDisplayZoneRecommendedViewSizeDXR",
                                        (PFN_xrVoidFunction*)&z.pfnViewSize)) ||
        z.pfnViewSize == nullptr) {
        ZONE_LOG("xrGetDisplayZoneRecommendedViewSizeDXR not resolvable "
                 "-- plain frames");
        return false;
    }

    XrDisplayZoneCapabilitiesDXR caps = {(XrStructureType)XR_TYPE_DISPLAY_ZONE_CAPABILITIES_DXR};
    XrResult cr = pfnCaps(session, &caps);
    if (XR_FAILED(cr) || !caps.supported || caps.maxZones3D < 1) {
        // Expected on non-window-bound sessions (headless/null compositor) —
        // supported=false there by design, not an error.
        ZONE_LOG("session not zone-capable (result=0x%x supported=%d "
                 "maxZones3D=%u) -- plain frames",
                 (unsigned)cr, (int)caps.supported, caps.maxZones3D);
        return false;
    }

    z.available = true;
    ZONE_LOG("active (maxZones3D=%u) -- zones-by-default frames", caps.maxZones3D);
    return true;
}

const XrDisplayZoneDXR*
FullWindowZoneLocateChain(FullWindowZone& z, uint32_t windowW, uint32_t windowH, const void* rigNext)
{
    if (!z.available || windowW == 0 || windowH == 0) {
        return nullptr;
    }

    z.zone.type = (XrStructureType)XR_TYPE_DISPLAY_ZONE_DXR;
    z.zone.zoneId = 1;
    z.zone.rect.offset = {0, 0};
    z.zone.rect.extent = {(int32_t)windowW, (int32_t)windowH};
    z.zone.next = rigNext;

    // Re-query the recommended per-view size only when the rect changed (or
    // after Invalidate). A failed query is soft: keep the previous size —
    // stale-but-valid beats zero.
    if ((int32_t)windowW != z.lastW || (int32_t)windowH != z.lastH) {
        XrExtent2Di sz = {0, 0};
        if (z.pfnViewSize != nullptr &&
            XR_SUCCEEDED(z.pfnViewSize(z.session, &z.zone.rect, &sz)) &&
            sz.width > 0 && sz.height > 0) {
            z.recommendedViewSize = sz;
        }
        z.lastW = (int32_t)windowW;
        z.lastH = (int32_t)windowH;
    }

    return &z.zone;
}

const XrDisplayZoneDXR*
FullWindowZoneSubmitChain(FullWindowZone& z)
{
    if (!z.available) {
        return nullptr;
    }
    // Same instance as the locate (the spec's dual-chain-point contract), rig
    // chain cleared — the submit side carries the zone only.
    z.zone.next = nullptr;
    return &z.zone;
}

void
FullWindowZoneInvalidate(FullWindowZone& z)
{
    z.lastW = z.lastH = -1;
}

}  // namespace dxr

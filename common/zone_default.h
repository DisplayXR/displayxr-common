// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief Zones-by-default: one full-window XrDisplayZoneDXR, shared plumbing.
 *
 * ADR-027's degenerate single-zone case (spec §6): a zones frame whose only 3D
 * zone is the full client window. Behaviourally equivalent to the legacy
 * single-canvas frame for the 3D content, but it buys the zones-frame
 * composition rules — which is the whole point:
 *
 *  - Local2D layers are pure 2D-over content composited POST-weave, so
 *    transient overlays (toasts, chips) escape the transparency-silhouette
 *    alpha gate that clips window-space layers stamped into the atlas.
 *  - Canvas authority is the (constant) zone rect. On a LEGACY frame it is
 *    the #439 supersede rule instead, keyed on "did this frame carry a
 *    Local2D layer" — so a transient Local2D overlay flips frame
 *    classification with every appearance. The zones frame makes overlay
 *    presence and canvas authority orthogonal, as they should be.
 *  - The hardware wish auto-derives from the ZONE rects (full window = all
 *    3D, stable); 2D rects never feed it, so a 2-second toast cannot flick a
 *    physical panel region 2D and back.
 *
 * Known trade: zones frames forfeit zero-copy (zone assembly is a composite,
 * so the full-fill handoff can never apply). Zero-copy only ever fires in the
 * worst-case-filling full-screen 2D mode (ADR-030) — one extra copy exactly
 * where perf matters least.
 *
 * Pure OpenXR, no platform dependencies. The APP still owns:
 *  - enabling XR_DXR_display_zones (+ XR_DXR_local_3d_zone >= v3) at
 *    xrCreateInstance — a helper cannot retrofit extensions;
 *  - chaining a rig descriptor if it uses one (pass it as `rigNext`);
 *  - falling back to its plain-frame path when Init() reports unsupported
 *    (headless/null-compositor sessions report supported = false).
 *
 * Usage:
 *
 *     static dxr::FullWindowZone g_zone;                       // app-global
 *     dxr::FullWindowZoneInit(g_zone, instance, session);      // once
 *     ...
 *     // per locate (rig chained through the zone):
 *     locateInfo.next = dxr::FullWindowZoneLocateChain(g_zone, winW, winH, &rig);
 *     // per xrEndFrame (SAME instance, rig chain cleared):
 *     proj.next = dxr::FullWindowZoneSubmitChain(g_zone);
 *
 * Both chain calls return nullptr on the fallback path, so `next` wiring
 * degrades to the plain frame with no app-side branching.
 */

#pragma once

#include <openxr/openxr.h>
#include <openxr/XR_DXR_display_zones.h>

#include <stdint.h>

namespace dxr {

struct FullWindowZone {
    //! Caps said yes and the entry points resolved; chain calls return the zone.
    bool available = false;
    //! The zone. Valid to read after LocateChain(); rect is the client window.
    XrDisplayZoneDXR zone{(XrStructureType)XR_TYPE_DISPLAY_ZONE_DXR, nullptr, 1, {{0, 0}, {0, 0}}};
    //! Recommended per-view image size for the current rect under the current
    //! display mode — re-queried whenever the rect (or mode, via the metrics
    //! event → call FullWindowZoneInvalidate) changes. {0,0} until first query.
    XrExtent2Di recommendedViewSize{0, 0};

    // Internal.
    PFN_xrGetDisplayZoneRecommendedViewSizeDXR pfnViewSize = nullptr;
    XrSession session = XR_NULL_HANDLE;
    int32_t lastW = -1, lastH = -1;
};

/*!
 * Resolve the extension entry points and query capabilities once. Returns the
 * resulting `available`. Never fails hard: an old runtime (extension absent) or
 * an unsupported session (headless) leaves `available` false and the chain
 * calls returning nullptr — the app's plain-frame path just keeps working.
 */
bool FullWindowZoneInit(FullWindowZone& z, XrInstance instance, XrSession session);

/*!
 * Update the zone rect to the client window and return the pointer to chain on
 * XrViewLocateInfo::next (nullptr when unavailable). `rigNext` (may be null) is
 * chained on the zone, so an XrDisplayRigDXR / XrCameraRigDXR rides the same
 * locate exactly as on the legacy path. Re-queries recommendedViewSize when the
 * rect changed (cheap dirty-check, not per frame).
 */
const XrDisplayZoneDXR* FullWindowZoneLocateChain(FullWindowZone& z,
                                                  uint32_t windowW, uint32_t windowH,
                                                  const void* rigNext);

/*!
 * Return the pointer to chain on XrCompositionLayerProjection::next at
 * xrEndFrame (nullptr when unavailable). Clears the rig chain first — the
 * submit chain must carry the zone only, and it must be the SAME instance the
 * locate used (the spec's dual-chain-point contract).
 */
const XrDisplayZoneDXR* FullWindowZoneSubmitChain(FullWindowZone& z);

/*!
 * Force the next LocateChain() to re-query recommendedViewSize even for an
 * unchanged rect — call on XR_TYPE_EVENT_DATA_DISPLAY_ZONE_METRICS_CHANGED_DXR
 * (display-mode / tile-count switch). Stale sizes stay correct, just soft.
 */
void FullWindowZoneInvalidate(FullWindowZone& z);

}  // namespace dxr

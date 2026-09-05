// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief Resolve near/far/clipFar from the runtime's advisory rear depth
 *        budget (XR_DXR_depth_budget) — DisplayXR/displayxr-common#38.
 *
 * Every transparent-background app hand-rolled the same rule: hard-clip the
 * far plane at the zero-disparity plane (ZDP, the display surface) whenever
 * the session is transparent AND standalone (no workspace controller behind
 * it). The runtime now computes an advisory, time-ramped "rear depth budget"
 * per session (see the rear-depth-budget design brief, runtime PR #1366) and
 * hands it to the app through @ref XrRearDepthBudgetDXR, chained on
 * XrViewState::next beside XrViewDisplayRawDXR in xrLocateViews. This header
 * is the ONE place that turns that budget (or its absence) into the near/far
 * planes and the shader/rasterizer far-cull value — so apps stop re-deriving
 * the policy.
 *
 * Division of labour, unchanged: the runtime owns the rear-depth POLICY, the
 * display processor owns PIXELS, the app owns GEOMETRY. This helper is pure
 * geometry — it does not call OpenXR, does not touch any global state, and
 * does not log. It also does NOT smooth: the runtime already time-ramps
 * farOffsetVH (ease-out, ~300 ms open / ~150 ms close) so the app's far plane
 * glides rather than pops — a second filter here would fight that ramp.
 * Apply the resolved value as-is, every frame.
 *
 * Fallback contract: `budget == nullptr` covers every case where the app has
 * no advisory value to read — the runtime predates XR_DXR_depth_budget, the
 * app did not enable the extension, or the app chose not to chain the struct
 * for this locate. In every one of those cases ResolveClipPlanes reproduces
 * today's rule bit-for-bit: `farOffsetVH = (transparent && standalone) ? 0 :
 * 1000`. An app that has ChainRearDepthBudget()'d the struct but wants to
 * tell "the runtime hasn't touched this yet" from "the runtime deliberately
 * returned an all-zero-vH clip" should check `budget->type ==
 * XR_TYPE_REAR_DEPTH_BUDGET_DXR` itself before passing a non-null pointer in
 * — a runtime that recognizes the chained struct always overwrites `type`
 * with that value (see the extension header), so an untouched
 * ChainRearDepthBudget() zero-init is distinguishable by that field alone.
 *
 * Usage (once per session):
 *
 *     XrRearDepthBudgetDXR budgetStorage;
 *     dxr::ChainRearDepthBudget(viewState, budgetStorage);   // before xrLocateViews
 *     ...
 *     xrLocateViews(session, &locateInfo, &viewState, ...);
 *     const XrRearDepthBudgetDXR* budget =
 *         (budgetStorage.type == XR_TYPE_REAR_DEPTH_BUDGET_DXR) ? &budgetStorage : nullptr;
 *     dxr::ClipPlanes clip = dxr::ResolveClipPlanes(ez, vHeight, budget, transparent, standalone);
 *     // clip.near_z / clip.far_z -> projection matrix; clip.clipFar -> shader/rasterizer cull.
 */
#pragma once

#include <openxr/openxr.h>
#include <openxr/XR_DXR_depth_budget.h>

namespace dxr {

//! Resolved clip planes + the far-offset that produced them (diagnostic /
//! HUD use — the demos' existing HUD lines already print this number).
struct ClipPlanes {
    float near_z = 0.0f;
    float far_z = 0.0f;
    float clipFar = 0.0f;     //!< shader/rasterizer far cull distance; 0 = no cull
    float farOffsetVH = 0.0f; //!< the farOffsetVH actually used (budget's, or the fallback)
};

/*!
 * Resolve this eye's near/far clip planes and shader far-cull from the
 * runtime's rear depth budget (or the pre-#38 fallback rule when there is
 * none).
 *
 * @param ez           Per-eye perpendicular distance to the display plane
 *                      (RigLocalEyeZ / display3d_view's eye_display.z), in
 *                      world/app units.
 * @param vH            Virtual display height, in the same units as ez.
 * @param budget        The XrRearDepthBudgetDXR the app chained on
 *                      XrViewState::next for this locate, or nullptr when the
 *                      runtime does not support XR_DXR_depth_budget, the app
 *                      did not enable it, or the chained struct came back
 *                      untouched (see the file comment above).
 * @param transparent   The session's own transparent-background state.
 * @param standalone    True when NOT running under a workspace controller
 *                      (today's gate for the hard ZDP clip).
 *
 * Math (design brief §2.6):
 *   near_z      = max(ez - vH, 1e-4)
 *   farOffsetVH = budget ? budget->farOffsetVH : ((transparent && standalone) ? 0 : 1000)
 *   far_z       = max(ez + farOffsetVH * vH, near_z + 1e-4)
 *   clipFar     = (transparent && farOffsetVH < 1000 && ez > 0.2) ? far_z : 0
 *
 * The `ez > 0.2f` guard is the demos' existing "never cull at/behind the near
 * plane" safety: at a near-degenerate eye distance the hard-clip math can
 * otherwise cull content the app still needs to draw.
 */
inline ClipPlanes
ResolveClipPlanes(float ez, float vH, const XrRearDepthBudgetDXR *budget, bool transparent, bool standalone)
{
    ClipPlanes out{};

    out.farOffsetVH = budget ? budget->farOffsetVH : ((transparent && standalone) ? 0.0f : 1000.0f);

    out.near_z = ez - vH;
    if (out.near_z < 1.0e-4f) {
        out.near_z = 1.0e-4f;
    }

    out.far_z = ez + out.farOffsetVH * vH;
    if (out.far_z < out.near_z + 1.0e-4f) {
        out.far_z = out.near_z + 1.0e-4f;
    }

    out.clipFar = (transparent && out.farOffsetVH < 1000.0f && ez > 0.2f) ? out.far_z : 0.0f;

    return out;
}

/*!
 * Zero-init `out` with the correct `type` and chain it onto `vs.next`,
 * preserving whatever chain XrViewState already carries (e.g. an app that
 * also chains its own diagnostics struct there). Call once per XrViewState
 * before xrLocateViews; the runtime overwrites `out` in place.
 *
 * Always succeeds — there is no failure path (this is pointer bookkeeping,
 * not an OpenXR call). The bool return exists for symmetry with this
 * library's other Init()-style helpers and is reserved for future use.
 */
inline bool
ChainRearDepthBudget(XrViewState &vs, XrRearDepthBudgetDXR &out)
{
    out = XrRearDepthBudgetDXR{};
    out.type = (XrStructureType)XR_TYPE_REAR_DEPTH_BUDGET_DXR;
    out.next = vs.next;
    vs.next = &out;
    return true;
}

//! Human-readable state name for HUD / log lines. Never returns null.
inline const char *
RearDepthBudgetStateName(XrRearDepthBudgetStateDXR state)
{
    switch (state) {
    case XR_REAR_DEPTH_BUDGET_STATE_UNRESTRICTED_OPAQUE_DXR: return "UnrestrictedOpaque";
    case XR_REAR_DEPTH_BUDGET_STATE_UNRESTRICTED_WORKSPACE_DXR: return "UnrestrictedWorkspace";
    case XR_REAR_DEPTH_BUDGET_STATE_OPEN_DXR: return "Open";
    case XR_REAR_DEPTH_BUDGET_STATE_CLIPPED_BUSY_BACKGROUND_DXR: return "ClippedBusyBackground";
    case XR_REAR_DEPTH_BUDGET_STATE_CLIPPED_NO_SOURCE_DXR: return "ClippedNoSource";
    case XR_REAR_DEPTH_BUDGET_STATE_FORCED_DXR: return "Forced";
    default: return "Unknown";
    }
}

} // namespace dxr

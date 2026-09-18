// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-041 "Model E": submit the LOCATED view count, alias the inactive tail.
 *
 * The runtime contract (displayxr-runtime PR #1533 / ADR-041): an
 * XrCompositionLayerProjection must carry EXACTLY the number of views
 * xrLocateViews returned, under every view configuration type. The view count
 * is fixed for the session's lifetime; what moves per frame is how many of
 * those views the active rendering mode uses — published as `activeViewCount`
 * on XrViewActivityStateDXR (XR_DXR_display_info SPEC_VERSION 21), chained on
 * XrViewState at xrLocateViews.
 *
 *   - views [0, active)  carry the active mode's poses/FOVs — the app renders these.
 *   - views [active, located) are INACTIVE. The runtime locates them at view 0's
 *     pose and IGNORES whatever pixels they point at.
 *
 * An app that renders only the active views therefore closes the gap by
 * ALIASING: each inactive view keeps its own located pose/fov but takes view
 * 0's subImage. DxrAliasInactiveViews() (common/dxr_view_config.h, vendored
 * from the runtime) is the reference tail fill; this header adds the two
 * pieces a *library* needs around it:
 *
 *   DxrReadViewActivity()     — read activeViewCount out of the chained struct,
 *                               falling back to a caller-supplied count (the
 *                               rendering-mode table's viewCount) when the
 *                               runtime did not fill it.
 *   DxrBuildProjectionViews() — copy the app's filled views into a
 *                               located-sized array and alias the tail, so the
 *                               caller's array may still be sized to the ACTIVE
 *                               count and stay const.
 *
 * Why the second one exists: EndFrame() takes the app's projection views as a
 * `const` pointer to an array the app sized to what it rendered. It cannot
 * alias in place (const, and possibly too short), so it stages into its own
 * buffer. That staging is the whole behavioural change, and it is header-only
 * and platform-neutral on purpose — the macOS / Linux / Android legs cannot
 * link the STATIC displayxr_common_lib but get this through displayxr::rules.
 *
 * Deprecated path this replaces: under PRIMARY_STEREO a 1-view submission in a
 * 1-view mode is still accepted by the runtime for one release, with a one-shot
 * WARN, then rejected. Under PRIMARY_MULTIVIEW_DXR it is rejected immediately.
 */

#pragma once

#include <stdint.h>

#include <openxr/openxr.h>

#include "dxr_view_config.h"  // DxrAliasInactiveViews() + the display_info header probe

/*
 * XrViewActivityStateDXR arrives with XR_DXR_display_info SPEC_VERSION 21.
 * Same two-stage reasoning as dxr_view_config.h: consumers pin their own
 * vendored openxr_includes/ and a pre-21 snapshot must not break the build.
 * The gate is the struct-type MACRO (the runtime header #defines it rather
 * than extending the XrStructureType enum), so this cannot shadow a future
 * real enumerator — if the header defines it, we define nothing.
 */
#ifndef XR_TYPE_VIEW_ACTIVITY_STATE_DXR
#define XR_TYPE_VIEW_ACTIVITY_STATE_DXR ((XrStructureType)1004999213)
#define DXR_VIEW_SUBMISSION_ACTIVITY_FALLBACK 1
typedef struct XrViewActivityStateDXR {
	XrStructureType type;            //!< Must be XR_TYPE_VIEW_ACTIVITY_STATE_DXR
	void *XR_MAY_ALIAS next;
	uint32_t activeViewCount;        //!< Views [0, activeViewCount) are live; the rest are inactive aliases
} XrViewActivityStateDXR;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Initialise @p activity and PREPEND it to @p viewState's chain, ready for
 * xrLocateViews. `activeViewCount` is pre-seeded to 0 so a runtime that does
 * not know the struct leaves a value the reader below can recognise as absent.
 *
 * Safe on a viewState that already carries a chain (e.g. the eye-tracking
 * state): the existing head is kept as this struct's `next`.
 */
static inline void
DxrChainViewActivity(XrViewState *viewState, XrViewActivityStateDXR *activity)
{
	if (viewState == NULL || activity == NULL) {
		return;
	}
	activity->type = (XrStructureType)XR_TYPE_VIEW_ACTIVITY_STATE_DXR;
	activity->next = (void *)viewState->next;
	activity->activeViewCount = 0;
	viewState->next = (void *)activity;
}

/*!
 * Read the active view count back after xrLocateViews.
 *
 * @param activity   The struct chained by DxrChainViewActivity(), or NULL.
 * @param located    What xrLocateViews returned (viewCountOutput).
 * @param fallback   What to use when the runtime did not fill the struct —
 *                   the active rendering mode's viewCount from the mode table.
 *                   Pass 0 to mean "then assume every located view is active".
 *
 * Never returns 0 (a frame always has at least view 0) and never returns more
 * than @p located.
 */
static inline uint32_t
DxrReadViewActivity(const XrViewActivityStateDXR *activity, uint32_t located, uint32_t fallback)
{
	uint32_t active = 0;

	if (activity != NULL && activity->activeViewCount > 0) {
		active = activity->activeViewCount;
	} else if (fallback > 0) {
		active = fallback;
	} else {
		active = located;
	}

	if (located > 0 && active > located) {
		active = located;
	}
	if (active == 0) {
		active = 1;
	}
	return active;
}

/*!
 * Stage a conformant projection-view array: the LOCATED count, with
 * [activeHint, located) aliased onto view 0's subImage.
 *
 * @param out          Destination, at least @p outCapacity entries.
 * @param outCapacity  Entries available in @p out (8 covers XRT_MAX_VIEWS).
 * @param filled       The views the app rendered and filled. May be NULL only
 *                     when @p activeHint is 0.
 * @param activeHint   How many entries of @p filled are valid. This is the
 *                     app's own active count (it knows it from the mode). It
 *                     can only ever SHRINK how much is copied, never how much
 *                     is submitted.
 * @param locatedViews The XrView array xrLocateViews wrote, so each inactive
 *                     view keeps its own pose/fov. NULL falls back to view 0's.
 * @param located      What xrLocateViews returned. 0 means "unknown" — the
 *                     function then submits @p activeHint unchanged, which is
 *                     the pre-ADR-041 behaviour and the only non-conformant
 *                     path left (it cannot know better).
 *
 * @return The number of views written to @p out — what the caller must put in
 *         XrCompositionLayerProjection::viewCount.
 */
static inline uint32_t
DxrBuildProjectionViews(XrCompositionLayerProjectionView *out,
                        uint32_t outCapacity,
                        const XrCompositionLayerProjectionView *filled,
                        uint32_t activeHint,
                        const XrView *locatedViews,
                        uint32_t located)
{
	uint32_t submit = 0;
	uint32_t active = 0;
	uint32_t i;

	if (out == NULL || outCapacity == 0) {
		return 0;
	}

	/*
	 * The located count is authoritative for the SUBMITTED size. An app
	 * that hands us fewer views than were located is the normal case (it
	 * rendered only the active ones) — and an app that hands us MORE than
	 * were located has over-rendered; copying only `located` of them is
	 * what the runtime accepts.
	 */
	submit = (located > 0) ? located : activeHint;
	if (submit > outCapacity) {
		submit = outCapacity;
	}

	active = activeHint;
	if (active > submit) {
		active = submit;
	}
	if (filled == NULL) {
		active = 0;
	}

	for (i = 0; i < active; i++) {
		out[i] = filled[i];
	}
	if (active == 0) {
		return 0; /* nothing to alias FROM — caller has no layer to submit */
	}

	DxrAliasInactiveViews(out, locatedViews, submit, active);
	return submit;
}

#ifdef __cplusplus
}
#endif

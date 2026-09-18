// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Contract test for common/view_submission.h (ADR-041, runtime #1533).
 *
 * The submission rule this pins is the one the runtime now enforces: an
 * XrCompositionLayerProjection carries EXACTLY the number of views
 * xrLocateViews returned. An app that renders only the ACTIVE views satisfies
 * it by aliasing the inactive tail onto view 0's subImage while each inactive
 * view keeps its OWN located pose/fov.
 *
 * This is the platform-neutral core of what EndFrame() / EndFrameWithWindowSpaceLayers()
 * / EndFrameWithQuadLayer() now do in xr_session_common.cpp — extracted into a
 * header precisely so it is testable on a runner with no OpenXR loader, no GPU
 * and no Win32 (xr_session_common.cpp is Windows-only).
 *
 * Runs as a TU of displayxr_view_config_test; view_config_test.c's main()
 * calls dxr_view_submission_check().
 */

#include <cstdio>
#include <cstring>

#include "view_submission.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                                                               \
	do {                                                                                                           \
		if (!(cond)) {                                                                                         \
			std::fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__);                         \
			g_failures++;                                                                                  \
		}                                                                                                      \
	} while (0)

namespace {

//! A distinct swapchain handle + rect per view, so an aliased tail is
//! unmistakable: view i's tile starts at x = 100 * i.
XrCompositionLayerProjectionView
MakeFilled(uint32_t i)
{
	XrCompositionLayerProjectionView v{};
	v.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
	v.next = nullptr;
	v.pose.position = {1.0f * (float)i, 0.0f, 0.0f};
	v.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
	v.fov = {-0.1f * (float)(i + 1), 0.1f * (float)(i + 1), 0.1f * (float)(i + 1), -0.1f * (float)(i + 1)};
	v.subImage.swapchain = (XrSwapchain)(uintptr_t)(0x1000 + i);
	v.subImage.imageRect.offset = {(int32_t)(100 * i), 0};
	v.subImage.imageRect.extent = {100, 200};
	v.subImage.imageArrayIndex = i;
	return v;
}

//! What xrLocateViews wrote — deliberately DIFFERENT numbers from MakeFilled,
//! so "kept its own located pose/fov" cannot pass by accident.
XrView
MakeLocated(uint32_t i)
{
	XrView v{};
	v.type = XR_TYPE_VIEW;
	v.pose.position = {0.0f, 10.0f * (float)i, 0.0f};
	v.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
	v.fov = {-0.5f - (float)i, 0.5f + (float)i, 0.5f + (float)i, -0.5f - (float)i};
	return v;
}

bool
SameSubImage(const XrSwapchainSubImage &a, const XrSwapchainSubImage &b)
{
	return a.swapchain == b.swapchain && a.imageArrayIndex == b.imageArrayIndex &&
	       a.imageRect.offset.x == b.imageRect.offset.x && a.imageRect.offset.y == b.imageRect.offset.y &&
	       a.imageRect.extent.width == b.imageRect.extent.width &&
	       a.imageRect.extent.height == b.imageRect.extent.height;
}

bool
SamePose(const XrPosef &a, const XrPosef &b)
{
	return a.position.x == b.position.x && a.position.y == b.position.y && a.position.z == b.position.z &&
	       a.orientation.x == b.orientation.x && a.orientation.y == b.orientation.y &&
	       a.orientation.z == b.orientation.z && a.orientation.w == b.orientation.w;
}

bool
SameFov(const XrFovf &a, const XrFovf &b)
{
	return a.angleLeft == b.angleLeft && a.angleRight == b.angleRight && a.angleUp == b.angleUp &&
	       a.angleDown == b.angleDown;
}

} // namespace

extern "C" int
dxr_view_submission_check(void)
{
	g_failures = 0;

	// ---- The v21 struct is available whatever snapshot is on the path ----
	CHECK((uint64_t)XR_TYPE_VIEW_ACTIVITY_STATE_DXR == 1004999213u,
	      "XR_TYPE_VIEW_ACTIVITY_STATE_DXR carries the DXR author-ID block value");
	{
		XrViewActivityStateDXR probe{};
		probe.activeViewCount = 3;
		CHECK(probe.activeViewCount == 3, "XrViewActivityStateDXR is a usable type");
	}

	// ---- Chaining preserves an existing XrViewState chain ----------------
	{
		XrViewState viewState{};
		viewState.type = XR_TYPE_VIEW_STATE;
		int sentinel = 0;
		viewState.next = &sentinel;

		XrViewActivityStateDXR activity{};
		DxrChainViewActivity(&viewState, &activity);
		CHECK(viewState.next == &activity, "activity struct becomes the chain head");
		CHECK(activity.next == &sentinel, "the previous chain head is preserved");
		CHECK(activity.type == (XrStructureType)XR_TYPE_VIEW_ACTIVITY_STATE_DXR, "type is stamped");
		CHECK(activity.activeViewCount == 0, "activeViewCount is pre-seeded to 0 = 'runtime did not fill it'");
	}

	// ---- Reading it back -------------------------------------------------
	{
		XrViewActivityStateDXR activity{};
		activity.activeViewCount = 2;
		CHECK(DxrReadViewActivity(&activity, 4, 1) == 2, "runtime value wins over the mode-table fallback");

		activity.activeViewCount = 0; // pre-v21 runtime: struct left untouched
		CHECK(DxrReadViewActivity(&activity, 4, 1) == 1, "unfilled → mode-table fallback");
		CHECK(DxrReadViewActivity(nullptr, 4, 2) == 2, "no struct → mode-table fallback");
		CHECK(DxrReadViewActivity(nullptr, 4, 0) == 4, "no struct and no fallback → every located view is active");

		activity.activeViewCount = 9;
		CHECK(DxrReadViewActivity(&activity, 4, 0) == 4, "never more than the located count");
	}

	// ---- located 4 / active 2 → submit 4, tail aliased -------------------
	{
		XrCompositionLayerProjectionView filled[2] = {MakeFilled(0), MakeFilled(1)};
		XrView located[4] = {MakeLocated(0), MakeLocated(1), MakeLocated(2), MakeLocated(3)};
		XrCompositionLayerProjectionView out[8]{};

		uint32_t n = DxrBuildProjectionViews(out, 8, filled, 2, located, 4);
		CHECK(n == 4, "located 4 / active 2 → layer viewCount 4");

		CHECK(SameSubImage(out[0].subImage, filled[0].subImage), "view 0 keeps its own tile");
		CHECK(SameSubImage(out[1].subImage, filled[1].subImage), "view 1 keeps its own tile");
		CHECK(SamePose(out[1].pose, filled[1].pose), "view 1 keeps its rendered pose");

		CHECK(SameSubImage(out[2].subImage, filled[0].subImage), "view 2 aliases view 0's subImage");
		CHECK(SameSubImage(out[3].subImage, filled[0].subImage), "view 3 aliases view 0's subImage");
		CHECK(out[2].type == XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW, "aliased view 2 is a valid struct");
		CHECK(out[3].type == XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW, "aliased view 3 is a valid struct");
		CHECK(out[2].next == nullptr && out[3].next == nullptr, "aliased views carry no stale chain");

		CHECK(SamePose(out[2].pose, located[2].pose), "view 2 keeps its OWN located pose");
		CHECK(SameFov(out[2].fov, located[2].fov), "view 2 keeps its OWN located fov");
		CHECK(SamePose(out[3].pose, located[3].pose), "view 3 keeps its OWN located pose");
		CHECK(SameFov(out[3].fov, located[3].fov), "view 3 keeps its OWN located fov");
		CHECK(!SamePose(out[2].pose, out[0].pose), "the tail is NOT collapsed onto view 0's pose");
	}

	// ---- located 2 / active 1 → submit 2 --------------------------------
	{
		XrCompositionLayerProjectionView filled[1] = {MakeFilled(0)};
		XrView located[2] = {MakeLocated(0), MakeLocated(1)};
		XrCompositionLayerProjectionView out[8]{};

		uint32_t n = DxrBuildProjectionViews(out, 8, filled, 1, located, 2);
		CHECK(n == 2, "located 2 / active 1 → layer viewCount 2");
		CHECK(SameSubImage(out[1].subImage, filled[0].subImage), "the 2D-mode tail aliases view 0");
		CHECK(SamePose(out[1].pose, located[1].pose), "the 2D-mode tail keeps view 1's located pose");
	}

	// ---- an OLD-STYLE call passing viewCount = 1 still submits 2 --------
	//
	// This is the whole back-compat claim: every shipped demo passes the ACTIVE
	// mode's count, and none of them has to change for the layer to become
	// conformant. Same inputs as above stated from the call site's point of
	// view: the app filled one view and said "1".
	{
		XrCompositionLayerProjectionView filled[2] = {MakeFilled(0), MakeFilled(1)};
		XrView located[2] = {MakeLocated(0), MakeLocated(1)};
		XrCompositionLayerProjectionView out[8]{};

		uint32_t n = DxrBuildProjectionViews(out, 8, filled, /*activeHint=*/1, located, 2);
		CHECK(n == 2, "old-style viewCount=1 → still submits the located 2");
		CHECK(SameSubImage(out[1].subImage, filled[0].subImage),
		      "and view 1 is the alias, NOT the app's stale view-1 tile");
		CHECK(!SameSubImage(out[1].subImage, filled[1].subImage),
		      "the app's unrendered view-1 tile is deliberately not used");
	}

	// ---- active == located → untouched passthrough -----------------------
	{
		XrCompositionLayerProjectionView filled[2] = {MakeFilled(0), MakeFilled(1)};
		XrView located[2] = {MakeLocated(0), MakeLocated(1)};
		XrCompositionLayerProjectionView out[8]{};

		uint32_t n = DxrBuildProjectionViews(out, 8, filled, 2, located, 2);
		CHECK(n == 2, "active == located → 2");
		CHECK(SameSubImage(out[1].subImage, filled[1].subImage), "nothing is aliased when nothing is inactive");
		CHECK(SamePose(out[1].pose, filled[1].pose), "and the app's own pose survives");
	}

	// ---- the active hint can never GROW the submitted count --------------
	{
		XrCompositionLayerProjectionView filled[4] = {MakeFilled(0), MakeFilled(1), MakeFilled(2), MakeFilled(3)};
		XrView located[2] = {MakeLocated(0), MakeLocated(1)};
		XrCompositionLayerProjectionView out[8]{};

		uint32_t n = DxrBuildProjectionViews(out, 8, filled, 4, located, 2);
		CHECK(n == 2, "over-rendered app is clamped to the located count");
	}

	// ---- never located → verbatim passthrough (pre-ADR-041) --------------
	{
		XrCompositionLayerProjectionView filled[2] = {MakeFilled(0), MakeFilled(1)};
		XrCompositionLayerProjectionView out[8]{};

		uint32_t n = DxrBuildProjectionViews(out, 8, filled, 2, nullptr, 0);
		CHECK(n == 2, "located == 0 → submit what the caller passed");
		CHECK(SameSubImage(out[1].subImage, filled[1].subImage), "…verbatim");
	}

	// ---- degenerate inputs never produce a half-built layer --------------
	{
		XrCompositionLayerProjectionView out[8]{};
		XrView located[2] = {MakeLocated(0), MakeLocated(1)};
		CHECK(DxrBuildProjectionViews(out, 8, nullptr, 0, located, 2) == 0, "no views → nothing to submit");
		CHECK(DxrBuildProjectionViews(nullptr, 8, nullptr, 0, located, 2) == 0, "null destination → 0");
		CHECK(DxrBuildProjectionViews(out, 0, nullptr, 0, located, 2) == 0, "zero capacity → 0");
	}

	// ---- the destination capacity is respected ---------------------------
	{
		XrCompositionLayerProjectionView filled[1] = {MakeFilled(0)};
		XrView located[4] = {MakeLocated(0), MakeLocated(1), MakeLocated(2), MakeLocated(3)};
		XrCompositionLayerProjectionView out[2]{};

		uint32_t n = DxrBuildProjectionViews(out, 2, filled, 1, located, 4);
		CHECK(n == 2, "a 2-entry destination cannot be overrun by a 4-view location");
	}

	if (g_failures != 0) {
		std::fprintf(stderr, "dxr_view_submission_check: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("dxr_view_submission_check: all checks passed\n");
	return 0;
}

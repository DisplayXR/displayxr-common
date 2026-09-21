// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  C++17 half of the common/dxr_view_config.h contract test.
 *
 * Same header, other language: the Windows apps reach it from C++ through
 * XrSessionManager, the Android/Linux legs from C. This TU proves it compiles
 * and behaves identically as C++17 (the fake xrEnumerateViewConfigurations it
 * calls lives in view_config_test.c, hence the extern "C" linkage).
 */

#include "dxr_view_config.h"

extern "C" int
dxr_view_config_cxx_check(void)
{
	if (DxrViewConfigTypeName(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR) == nullptr) {
		return 1;
	}
	// main() leaves the fake runtime scripted to ADVERTISE the type here.
	if (DxrSelectViewConfigType((XrInstance)(uintptr_t)0x1, (XrSystemId)1) !=
	    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR) {
		return 1;
	}
	if (DxrSelectViewConfigType(XR_NULL_HANDLE, (XrSystemId)1) !=
	    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
		return 1;
	}
	// ADR-041 aliasing compiles and behaves the same from C++17.
	XrView views[2] = {};
	XrCompositionLayerProjectionView pv[2] = {};
	pv[0].subImage.swapchain = (XrSwapchain)(uintptr_t)0x7;
	DxrAliasInactiveViews(pv, views, 2, 1);
	if (pv[1].subImage.swapchain != pv[0].subImage.swapchain ||
	    pv[1].type != XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW) {
		return 1;
	}
	return 0;
}

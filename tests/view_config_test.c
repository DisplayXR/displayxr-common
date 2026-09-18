// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Contract test for common/dxr_view_config.h (runtime #1486).
 *
 * The header is the app-called opt-in to
 * XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR, so what has to hold is:
 *   - it compiles as C11 (this TU) and as C++17 (view_config_test_cxx.cpp),
 *   - the enumerator is defined whatever the consumer's header snapshot is,
 *   - the probe returns MULTIVIEW_DXR only when the runtime enumerates it, and
 *     degrades to PRIMARY_STEREO on every other path (old runtime, extension
 *     not enabled, enumerate failure, null handles).
 *
 * There is no OpenXR loader on the CI runners, so this TU DEFINES
 * xrEnumerateViewConfigurations itself — which is also what makes the probe
 * loop testable at all.
 */

#include <stdio.h>
#include <string.h>

#include "dxr_view_config.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                                                               \
	do {                                                                                                           \
		if (!(cond)) {                                                                                         \
			fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__);                              \
			g_failures++;                                                                                  \
		}                                                                                                      \
	} while (0)

/* Script for the fake runtime below. */
static XrViewConfigurationType g_types[8];
static uint32_t g_type_count = 0;
static XrResult g_enumerate_result = XR_SUCCESS;

XRAPI_ATTR XrResult XRAPI_CALL
xrEnumerateViewConfigurations(XrInstance instance,
                              XrSystemId systemId,
                              uint32_t viewConfigurationTypeCapacityInput,
                              uint32_t *viewConfigurationTypeCountOutput,
                              XrViewConfigurationType *viewConfigurationTypes)
{
	uint32_t i;
	(void)instance;
	(void)systemId;

	if (g_enumerate_result != XR_SUCCESS) {
		return g_enumerate_result;
	}
	if (viewConfigurationTypeCapacityInput == 0) {
		*viewConfigurationTypeCountOutput = g_type_count;
		return XR_SUCCESS;
	}
	if (viewConfigurationTypeCapacityInput < g_type_count) {
		return XR_ERROR_SIZE_INSUFFICIENT;
	}
	for (i = 0; i < g_type_count; i++) {
		viewConfigurationTypes[i] = g_types[i];
	}
	*viewConfigurationTypeCountOutput = g_type_count;
	return XR_SUCCESS;
}

/* Defined in view_config_test_cxx.cpp — proves the header is C++17-clean. */
int
dxr_view_config_cxx_check(void);

/* Defined in view_submission_test.cpp — the ADR-041 located-count submission
 * contract (common/view_submission.h). Lives in this target because it shares
 * the header chain, and because the C TU here is what makes the OpenXR symbols
 * link on a loader-free runner. */
int
dxr_view_submission_check(void);

static void
script(XrResult result, uint32_t count, XrViewConfigurationType a, XrViewConfigurationType b)
{
	g_enumerate_result = result;
	g_type_count = count;
	g_types[0] = a;
	g_types[1] = b;
}

/* Macros, not static consts: an integer cast to a handle is not a portable
 * static initialiser in C. */
#define kFakeInstance ((XrInstance)(uintptr_t)0x1)
#define kFakeSystem ((XrSystemId)1)

int
main(void)
{
	/*
	 * The enumerator is available regardless of which XR_DXR_display_info.h
	 * (if any) the consumer put on the include path — that is the whole
	 * point of the two-stage fallback in the header.
	 */
	CHECK((uint64_t)XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR == 1004999212u,
	      "PRIMARY_MULTIVIEW_DXR carries the DXR author-ID block value");
	CHECK(strcmp(DxrViewConfigTypeName(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR),
	             "PRIMARY_MULTIVIEW_DXR") == 0,
	      "name of PRIMARY_MULTIVIEW_DXR");
	CHECK(strcmp(DxrViewConfigTypeName(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO), "PRIMARY_STEREO") == 0,
	      "name of PRIMARY_STEREO");
	CHECK(strcmp(DxrViewConfigTypeName(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO), "other") == 0,
	      "name of an unhandled type");

	/* Runtime advertises it (XR_DXR_display_info enabled) → opt in. */
	script(XR_SUCCESS, 2, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
	       XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR);
	CHECK(DxrSelectViewConfigType(kFakeInstance, kFakeSystem) ==
	          XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR,
	      "advertised → PRIMARY_MULTIVIEW_DXR");

	/* Old runtime, or the extension was not enabled on the instance. */
	script(XR_SUCCESS, 2, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO,
	       XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);
	CHECK(DxrSelectViewConfigType(kFakeInstance, kFakeSystem) == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
	      "not advertised → PRIMARY_STEREO");

	/* Degenerate + error paths never fail the app. */
	script(XR_SUCCESS, 0, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);
	CHECK(DxrSelectViewConfigType(kFakeInstance, kFakeSystem) == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
	      "empty enumeration → PRIMARY_STEREO");

	script(XR_ERROR_RUNTIME_FAILURE, 2, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR,
	       XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);
	CHECK(DxrSelectViewConfigType(kFakeInstance, kFakeSystem) == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
	      "enumerate failure → PRIMARY_STEREO");

	script(XR_SUCCESS, 2, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR,
	       XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);
	CHECK(DxrSelectViewConfigType(XR_NULL_HANDLE, kFakeSystem) == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
	      "null instance → PRIMARY_STEREO");
	CHECK(DxrSelectViewConfigType(kFakeInstance, XR_NULL_SYSTEM_ID) == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
	      "null system → PRIMARY_STEREO");

	if (dxr_view_config_cxx_check() != 0) {
		fprintf(stderr, "FAIL: C++17 view of dxr_view_config.h disagrees with the C one\n");
		g_failures++;
	}

	if (dxr_view_submission_check() != 0) {
		fprintf(stderr, "FAIL: view_submission.h contract (ADR-041)\n");
		g_failures++;
	}

	if (g_failures != 0) {
		fprintf(stderr, "displayxr_view_config_test: %d failure(s)\n", g_failures);
		return 1;
	}
	printf("displayxr_view_config_test: all checks passed\n");
	return 0;
}

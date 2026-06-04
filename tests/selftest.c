// Copyright 2025-2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Tiny CI consumer for displayxr::math.
 *
 * Links displayxr::math and runs display3d_selftest() — the projection /
 * unproject round-trip + NDC-orientation drift guard built into the library.
 * A non-zero return means the math conventions have drifted; CI fails. This is
 * the smallest possible "does the library build, link, and stay correct"
 * harness referenced by W2.
 */

#include "display3d_view.h"

#include <stdio.h>

int
main(void)
{
	int failures = display3d_selftest();
	if (failures != 0) {
		fprintf(stderr, "display3d_selftest FAILED: %d check(s)\n", failures);
		return 1;
	}
	printf("display3d_selftest OK\n");
	return 0;
}

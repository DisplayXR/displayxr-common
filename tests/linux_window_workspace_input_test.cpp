// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The pure half of DxrLinuxWindow's workspace input
 *         (common/linux/dxr_workspace_input.h). No display, no runtime.
 *
 * 1. vk_to_keysym inverts the service's keysym -> VK table for every key the
 *    service forwards (letters to level-0 lower case, digits, F-keys, keypad,
 *    named keys, OEM punctuation).
 * 2. Buttons: the wire's right/middle numbering is swapped to the window's.
 * 3. uv_to_px: normalized content position -> buffer px, rounding, a drag
 *    that leaves the window is not clamped.
 * 4. Scroll: whole notches pass, a fractional delta still counts one step.
 */
#include "dxr_workspace_input.h"

#include <cstdio>

namespace W = dxr_wsinput;

static int g_fail = 0;
#define CHECK(c, msg)                                                                                                  \
	do {                                                                                                           \
		if (!(c)) {                                                                                            \
			std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                           \
			g_fail++;                                                                                      \
		}                                                                                                      \
	} while (0)

//! The service's table (runtime comp_window_linux_keysym_to_vk), copied so the
//! round trip is checked against what is actually sent.
static uint32_t
service_keysym_to_vk(uint32_t ks)
{
	if (ks >= 0x61 && ks <= 0x7a) return ks - 0x20;
	if (ks >= 0x30 && ks <= 0x39) return ks;
	if (ks >= 0xffbe && ks <= 0xffc9) return 0x70 + (ks - 0xffbe);
	if (ks >= 0xffb0 && ks <= 0xffb9) return 0x60 + (ks - 0xffb0);
	switch (ks) {
	case 0x0020: return 0x20;
	case 0xff08: return 0x08;
	case 0xff09: return 0x09;
	case 0xff0d: return 0x0D;
	case 0xff1b: return 0x1B;
	case 0xffff: return 0x2E;
	case 0xff63: return 0x2D;
	case 0xff50: return 0x24;
	case 0xff57: return 0x23;
	case 0xff55: return 0x21;
	case 0xff56: return 0x22;
	case 0xff51: return 0x25;
	case 0xff52: return 0x26;
	case 0xff53: return 0x27;
	case 0xff54: return 0x28;
	case 0xffe1: return 0x10;
	case 0xffe3: return 0x11;
	case 0xffe9: return 0x12;
	case 0xffeb: return 0x5B;
	case 0xffec: return 0x5C;
	case 0x003b: return 0xBA;
	case 0x003d: return 0xBB;
	case 0x002c: return 0xBC;
	case 0x002d: return 0xBD;
	case 0x002e: return 0xBE;
	case 0x002f: return 0xBF;
	case 0x0060: return 0xC0;
	case 0x005b: return 0xDB;
	case 0x005c: return 0xDC;
	case 0x005d: return 0xDD;
	case 0x0027: return 0xDE;
	case 0xffab: return 0x6B;
	case 0xffad: return 0x6D;
	default: return 0;
	}
}

int
main()
{
	// 1. Round trip over every keysym the service can send (left/primary
	// variants: the service folds Shift_R/Control_R/... onto one VK).
	const uint32_t named[] = {0x20,   0xff08, 0xff09, 0xff0d, 0xff1b, 0xffff, 0xff63, 0xff50, 0xff57,
	                          0xff55, 0xff56, 0xff51, 0xff52, 0xff53, 0xff54, 0xffe1, 0xffe3, 0xffe9,
	                          0xffeb, 0xffec, 0x3b,   0x3d,   0x2c,   0x2d,   0x2e,   0x2f,   0x60,
	                          0x5b,   0x5c,   0x5d,   0x27,   0xffab, 0xffad};
	for (uint32_t ks : named) {
		const uint32_t vk = service_keysym_to_vk(ks);
		CHECK(vk != 0 && W::vk_to_keysym(vk) == ks, "named key round-trips");
	}
	for (uint32_t ks = 0x61; ks <= 0x7a; ks++) {
		CHECK(W::vk_to_keysym(service_keysym_to_vk(ks)) == ks, "letter round-trips to level-0 lower case");
	}
	for (uint32_t ks = 0x30; ks <= 0x39; ks++) {
		CHECK(W::vk_to_keysym(service_keysym_to_vk(ks)) == ks, "digit round-trips");
	}
	for (uint32_t ks = 0xffbe; ks <= 0xffc9; ks++) {
		CHECK(W::vk_to_keysym(service_keysym_to_vk(ks)) == ks, "F-key round-trips");
	}
	for (uint32_t ks = 0xffb0; ks <= 0xffb9; ks++) {
		CHECK(W::vk_to_keysym(service_keysym_to_vk(ks)) == ks, "keypad digit round-trips");
	}
	CHECK(W::vk_to_keysym('T') == 0x74, "VK 'T' -> XK_t (the Ctrl+T case)");
	CHECK(W::vk_to_keysym(0xFF) == 0, "unknown VK -> 0");
	CHECK(W::keysym_to_keycode(0x74) == 20 + 8, "XK_t -> evdev KEY_T + 8");
	CHECK(W::keysym_to_keycode(0x30) == 11 + 8, "XK_0 -> KEY_0 + 8");

	// Modifiers pass through bit for bit (shift, ctrl, alt, super).
	CHECK(W::mods_from_wire(0x2) == 0x2, "CTRL bit");
	CHECK(W::mods_from_wire(0xF0 | 0x5) == 0x5, "unknown high bits dropped");

	// 2. Buttons.
	CHECK(W::button_from_wire(1) == 1, "left stays left");
	CHECK(W::button_from_wire(2) == 3, "wire right -> window 3");
	CHECK(W::button_from_wire(3) == 2, "wire middle -> window 2");
	CHECK(W::button_from_wire(9) == 0, "unknown button -> 0");

	// 3. Positions.
	int32_t x = 0, y = 0;
	W::uv_to_px(0.5f, 0.25f, 1920, 1080, &x, &y);
	CHECK(x == 960 && y == 270, "centre-ish maps to buffer px");
	W::uv_to_px(0.0f, 1.0f, 800, 600, &x, &y);
	CHECK(x == 0 && y == 600, "edges");
	W::uv_to_px(-0.1f, 1.1f, 1000, 1000, &x, &y);
	CHECK(x == -100 && y == 1100, "a drag outside the window is not clamped");

	// 4. Scroll.
	CHECK(W::scroll_steps_from_wire(1.0f) == 1, "one notch up");
	CHECK(W::scroll_steps_from_wire(-2.0f) == -2, "two notches down");
	CHECK(W::scroll_steps_from_wire(0.2f) == 1, "a small smooth scroll counts one step");
	CHECK(W::scroll_steps_from_wire(-0.2f) == -1, "and down");
	CHECK(W::scroll_steps_from_wire(0.0f) == 0, "no scroll");

	if (g_fail == 0) {
		std::printf("linux_window_workspace_input_test: all checks passed\n");
	}
	return g_fail == 0 ? 0 : 1;
}

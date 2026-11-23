// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Pure helpers behind DxrLinuxWindow's workspace input: the routed
 *         input a content app receives inside the DisplayXR workspace through
 *         XR_DXR_workspace_client_input, turned into the window's own event
 *         vocabulary. No window-system calls, so unit tested
 *         (tests/linux_window_workspace_input_test.cpp).
 *
 * WHY: inside the workspace (DISPLAYXR_WORKSPACE_SESSION=1) the app's window is
 * created hidden (DxrLinuxWindowDesc::start_hidden) and the service composites
 * its swapchain into the shared spatial surface. The pointer and keyboard are
 * on the SERVICE's surface, so the app's own window never sees an event. On
 * Windows the service posts each event into the app's hidden HWND; Wayland has
 * no way to post into another client's surface, so the runtime queues the
 * events for the app and the app drains them. DxrLinuxWindow drains them in
 * pump()/pump_events() and delivers them as ordinary DxrWindowEvents — an app
 * that handles its window's events needs no change beyond enabling the
 * extension.
 */
#pragma once

#include <cstdint>

#include <openxr/openxr.h>

// The extension's types, for builds whose extension headers predate it. The
// guard is the header's own feature macro, so a newer header wins.
#ifndef XR_DXR_workspace_client_input
#define XR_DXR_workspace_client_input 1
#define XR_DXR_workspace_client_input_SPEC_VERSION 1
#define XR_DXR_WORKSPACE_CLIENT_INPUT_EXTENSION_NAME "XR_DXR_workspace_client_input"
typedef enum XrWorkspaceClientInputEventTypeDXR
{
	XR_WORKSPACE_CLIENT_INPUT_EVENT_KEY_DXR = 0,
	XR_WORKSPACE_CLIENT_INPUT_EVENT_BUTTON_DXR = 1,
	XR_WORKSPACE_CLIENT_INPUT_EVENT_MOTION_DXR = 2,
	XR_WORKSPACE_CLIENT_INPUT_EVENT_SCROLL_DXR = 3,
	XR_WORKSPACE_CLIENT_INPUT_EVENT_TYPE_MAX_ENUM_DXR = 0x7FFFFFFF
} XrWorkspaceClientInputEventTypeDXR;
typedef struct XrWorkspaceClientInputEventDXR
{
	XrWorkspaceClientInputEventTypeDXR eventType;
	uint32_t timestampMs;
	uint32_t vkCode;
	XrBool32 isDown;
	uint32_t button;
	uint32_t buttonMask;
	uint32_t modifiers;
	float scrollDeltaY;
	XrBool32 positionValid;
	XrVector2f position;
} XrWorkspaceClientInputEventDXR;
typedef XrResult(XRAPI_PTR *PFN_xrEnumerateWorkspaceClientInputEventsDXR)(XrSession session,
                                                                          uint32_t capacityInput,
                                                                          uint32_t *countOutput,
                                                                          XrWorkspaceClientInputEventDXR *events);
#endif

namespace dxr_wsinput {

/*!
 * Win32 VK_* (what the workspace queue carries) -> the X11 keysym at shift
 * level 0 (what DxrWindowEvent::keysym carries). The inverse of the service's
 * comp_window_linux_keysym_to_vk(), so every key the service forwards maps
 * back. 0 = no keysym for it.
 */
inline uint32_t
vk_to_keysym(uint32_t vk)
{
	if (vk >= 0x41 && vk <= 0x5A) { // VK_A..VK_Z -> XK_a..XK_z (level 0)
		return vk + 0x20;
	}
	if (vk >= 0x30 && vk <= 0x39) { // VK_0..VK_9 == XK_0..XK_9
		return vk;
	}
	if (vk >= 0x70 && vk <= 0x7B) { // VK_F1..VK_F12
		return 0xffbe + (vk - 0x70);
	}
	if (vk >= 0x60 && vk <= 0x69) { // VK_NUMPAD0..9 -> XK_KP_0..9
		return 0xffb0 + (vk - 0x60);
	}
	switch (vk) {
	case 0x20: return 0x0020; // VK_SPACE
	case 0x08: return 0xff08; // VK_BACK      -> BackSpace
	case 0x09: return 0xff09; // VK_TAB
	case 0x0D: return 0xff0d; // VK_RETURN
	case 0x1B: return 0xff1b; // VK_ESCAPE
	case 0x2E: return 0xffff; // VK_DELETE
	case 0x2D: return 0xff63; // VK_INSERT
	case 0x24: return 0xff50; // VK_HOME
	case 0x23: return 0xff57; // VK_END
	case 0x21: return 0xff55; // VK_PRIOR     -> Prior (Page Up)
	case 0x22: return 0xff56; // VK_NEXT      -> Next (Page Down)
	case 0x25: return 0xff51; // VK_LEFT
	case 0x26: return 0xff52; // VK_UP
	case 0x27: return 0xff53; // VK_RIGHT
	case 0x28: return 0xff54; // VK_DOWN
	case 0x10: return 0xffe1; // VK_SHIFT     -> Shift_L
	case 0x11: return 0xffe3; // VK_CONTROL   -> Control_L
	case 0x12: return 0xffe9; // VK_MENU      -> Alt_L
	case 0x5B: return 0xffeb; // VK_LWIN      -> Super_L
	case 0x5C: return 0xffec; // VK_RWIN      -> Super_R
	case 0xBA: return 0x003b; // VK_OEM_1     -> semicolon
	case 0xBB: return 0x003d; // VK_OEM_PLUS  -> equal
	case 0xBC: return 0x002c; // VK_OEM_COMMA -> comma
	case 0xBD: return 0x002d; // VK_OEM_MINUS -> minus
	case 0xBE: return 0x002e; // VK_OEM_PERIOD-> period
	case 0xBF: return 0x002f; // VK_OEM_2     -> slash
	case 0xC0: return 0x0060; // VK_OEM_3     -> grave
	case 0xDB: return 0x005b; // VK_OEM_4     -> bracketleft
	case 0xDC: return 0x005c; // VK_OEM_5     -> backslash
	case 0xDD: return 0x005d; // VK_OEM_6     -> bracketright
	case 0xDE: return 0x0027; // VK_OEM_7     -> apostrophe
	case 0x6B: return 0xffab; // VK_ADD       -> KP_Add
	case 0x6D: return 0xffad; // VK_SUBTRACT  -> KP_Subtract
	default: return 0;
	}
}

/*!
 * X11/xkb keycode (evdev + 8) for a keysym, for the handful of apps that key
 * on DxrWindowEvent::keycode. Covers letters, digits and the common named
 * keys (US layout); 0 when unknown — apps should prefer the keysym.
 */
inline uint32_t
keysym_to_keycode(uint32_t ks)
{
	// evdev KEY_* codes, + 8.
	static const uint8_t letters[26] = {30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50,
	                                    49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44};
	if (ks >= 0x61 && ks <= 0x7a) {
		return letters[ks - 0x61] + 8u;
	}
	if (ks >= 0x31 && ks <= 0x39) { // 1..9 = KEY_1(2)..KEY_9(10)
		return (ks - 0x31) + 2u + 8u;
	}
	switch (ks) {
	case 0x30: return 11 + 8;   // 0
	case 0x20: return 57 + 8;   // space
	case 0xff1b: return 1 + 8;  // Escape
	case 0xff0d: return 28 + 8; // Return
	case 0xff09: return 15 + 8; // Tab
	case 0xff08: return 14 + 8; // BackSpace
	case 0xff51: return 105 + 8; // Left
	case 0xff52: return 103 + 8; // Up
	case 0xff53: return 106 + 8; // Right
	case 0xff54: return 108 + 8; // Down
	default: return 0;
	}
}

//! Wire modifier bits (bit0 SHIFT, bit1 CTRL, bit2 ALT, bit3 SUPER) ->
//! DxrKeyMod bits. Same layout today; kept as a function so a divergence is
//! one edit.
inline uint32_t
mods_from_wire(uint32_t m)
{
	return m & 0xFu;
}

//! Wire button (1 left, 2 right, 3 middle) -> DxrWindowEvent::button
//! (1 left, 2 middle, 3 right). 0 for anything else.
inline uint32_t
button_from_wire(uint32_t b)
{
	switch (b) {
	case 1: return 1;
	case 2: return 3;
	case 3: return 2;
	default: return 0;
	}
}

//! A normalized content position -> CONTENT buffer px for a @p w x @p h
//! content. Rounds to nearest; not clamped (a drag can leave the window).
inline void
uv_to_px(float u, float v, uint32_t w, uint32_t h, int32_t *out_x, int32_t *out_y)
{
	const double x = (double)u * (double)w;
	const double y = (double)v * (double)h;
	*out_x = (int32_t)(x < 0.0 ? x - 0.5 : x + 0.5);
	*out_y = (int32_t)(y < 0.0 ? y - 0.5 : y + 0.5);
}

//! Wheel notches (+ = up) -> DxrWindowEvent::scroll_steps (+1 per notch up).
//! Fractional deltas round away from zero so a small smooth scroll still
//! counts as one step.
inline int32_t
scroll_steps_from_wire(float delta)
{
	if (delta > 0.0f) {
		const int32_t s = (int32_t)(delta + 0.5f);
		return s > 0 ? s : 1;
	}
	if (delta < 0.0f) {
		const int32_t s = (int32_t)(delta - 0.5f);
		return s < 0 ? s : -1;
	}
	return 0;
}

} // namespace dxr_wsinput

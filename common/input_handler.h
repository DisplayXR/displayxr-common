// Copyright 2025, The DisplayXR Project and its contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Keyboard and mouse input state tracking
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <string>

#include "input_state.h" // struct InputState (platform-neutral; split out for gamepad_actions.h)

// Process a Win32 message and update input state
// Returns true if the message was handled
bool UpdateInputState(InputState& state, UINT msg, WPARAM wParam, LPARAM lParam);

// Update camera position based on current key states
// deltaTime is in seconds, displayHeightM is physical display height for m2v scaling
void UpdateCameraMovement(InputState& state, float deltaTime, float displayHeightM = 0.0f);

// Get a string describing current mouse button state
std::string GetMouseButtonString(const InputState& state);

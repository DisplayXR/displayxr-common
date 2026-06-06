// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Window creation and fullscreen management
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <string>

struct WindowInfo {
    HWND hwnd = nullptr;
    HINSTANCE hInstance = nullptr;

    // Window dimensions
    int width = 1280;
    int height = 720;

    // Monitor info
    int monitorX = 0;
    int monitorY = 0;
    int monitorWidth = 1920;
    int monitorHeight = 1080;

    // State
    bool fullscreen = false;

    // Saved windowed position (for toggling fullscreen)
    RECT savedWindowRect = {};
    LONG savedStyle = 0;
    LONG savedExStyle = 0;

    // Window class name
    std::wstring className;
};

// Create an application window
// If monitorIndex >= 0, places window on that specific monitor
bool CreateAppWindow(
    WindowInfo& info,
    HINSTANCE hInstance,
    const wchar_t* className,
    const wchar_t* title,
    int width,
    int height,
    WNDPROC wndProc,
    int monitorIndex = -1  // -1 = default monitor
);

// Find the SR display monitor and return its index
// Returns -1 if not found
int FindSRDisplayMonitor();

// Get monitor information by index
bool GetMonitorInfo(int monitorIndex, RECT& monitorRect);

// Toggle fullscreen mode
void ToggleFullscreen(WindowInfo& info);

// Enter fullscreen on the specified monitor
void EnterFullscreen(WindowInfo& info, int monitorIndex = -1);

// Exit fullscreen and restore windowed mode
void ExitFullscreen(WindowInfo& info);

// Destroy the window
void DestroyAppWindow(WindowInfo& info);

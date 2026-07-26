// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief Transient on-screen confirmation message ("toast") — shared state.
 *
 * A toast is a short, self-expiring message that confirms an action the user
 * just took (a mode toggle, a pin change, a capture). It is deliberately
 * INDEPENDENT of the HUD info panel: the HUD is a reference display the user
 * toggles with Tab, whereas a toast must be visible at the moment of the action
 * whether or not the HUD is up.
 *
 * This header is platform- and graphics-API-neutral: it owns only the message
 * and its lifetime. Rasterizing is the renderer's job —
 * RenderToastStandalone() (hud_renderer.h, Windows/D2D) draws the chip; a
 * caller on another backend can read Snapshot() and draw it any way it likes.
 *
 * Threading: Show() is typically called from the window/input thread and
 * Snapshot() from the render thread, so both take an internal lock.
 *
 * Typical use:
 *
 *     static dxr::ToastState g_toast;                  // app-global
 *     ...
 *     g_toast.Show(L"Recenter: PIN XY-");              // on the key press
 *     ...
 *     std::wstring msg; float a;                       // in the render loop
 *     if (g_toast.Snapshot(msg, a)) { ...draw chip at alpha a... }
 */

#pragma once

#include <chrono>
#include <mutex>
#include <string>

namespace dxr {

//! Window-space layer placement, in fractions of the window (the units
//! XrCompositionLayerWindowSpaceDXR::x/y/width/height take).
struct ToastLayerRect {
    float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
};

/*!
 * Place a toast chip of texture aspect `texAspect` (width/height) horizontally
 * centred at `yFraction` down the window.
 *
 * The chip is sized off the window's SHORTER side, NOT its width. Sizing off
 * width makes the same chip render large in a landscape window and small in a
 * portrait one — the demos hit exactly that (a landscape model viewer vs a
 * portrait avatar), which is the whole reason this lives in one place instead
 * of being re-derived per app.
 *
 * `sizeFraction` is the chip's width as a fraction of that shorter side. The
 * result is clamped so a very wide chip in a very narrow window still fits.
 */
ToastLayerRect ComputeToastLayerRect(uint32_t windowW, uint32_t windowH,
                                     float texAspect,
                                     float sizeFraction = 0.60f,
                                     float yFraction = 0.84f);

class ToastState {
public:
    // Seconds a toast stays up, and the tail of that window spent fading out.
    static constexpr float kDefaultSeconds = 2.0f;
    static constexpr float kFadeSeconds = 0.4f;

    //! Post a message. Replaces any toast still on screen (the newest action is
    //! the one the user wants confirmed) and restarts the clock.
    void Show(const std::wstring& text, float seconds = kDefaultSeconds);

    //! Retire the current toast immediately.
    void Clear();

    //! True while a toast is still on screen.
    bool Active() const;

    //! One locked read of both fields. Returns false (leaving the outputs
    //! untouched) when no toast is up. `outAlpha` is 1.0 for most of the
    //! lifetime and ramps to 0 over the last kFadeSeconds.
    bool Snapshot(std::wstring& outText, float& outAlpha) const;

private:
    using Clock = std::chrono::steady_clock;

    mutable std::mutex mutex_;
    std::wstring text_;
    Clock::time_point expiry_{};   //!< default-constructed == never shown
    float fadeSeconds_ = kFadeSeconds;
    bool active_ = false;
};

}  // namespace dxr

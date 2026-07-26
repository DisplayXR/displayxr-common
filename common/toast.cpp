// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief Transient on-screen confirmation message ("toast") — shared state.
 */

#include "toast.h"

namespace dxr {

ToastLayerRect ComputeToastLayerRect(uint32_t windowW, uint32_t windowH,
                                     float texAspect,
                                     float sizeFraction,
                                     float yFraction)
{
    ToastLayerRect r;
    if (windowW == 0 || windowH == 0 || texAspect <= 0.0f) {
        // Degenerate window (minimized / pre-first-resize): a centred nominal
        // chip is better than a divide-by-zero.
        r.x = 0.3f; r.y = yFraction; r.width = 0.4f; r.height = 0.06f;
        return r;
    }

    const float w = static_cast<float>(windowW);
    const float h = static_cast<float>(windowH);
    const float shortSide = (w < h) ? w : h;

    // Chip size in PIXELS first — that is what "looks the same" means to the
    // user — then convert to the window fractions the layer wants.
    float chipW = sizeFraction * shortSide;
    float chipH = chipW / texAspect;

    // Never let the chip crowd the window it annotates.
    const float maxW = 0.92f * w;
    if (chipW > maxW) { chipW = maxW; chipH = chipW / texAspect; }

    r.width = chipW / w;
    r.height = chipH / h;
    r.x = 0.5f - r.width * 0.5f;

    // Keep the whole chip on screen if yFraction sits it too low.
    r.y = yFraction;
    if (r.y + r.height > 1.0f) r.y = 1.0f - r.height;
    if (r.y < 0.0f) r.y = 0.0f;
    return r;
}

void ToastState::Show(const std::wstring& text, float seconds) {
    if (seconds <= 0.0f) seconds = kDefaultSeconds;
    // A fade longer than the toast itself would start it already transparent.
    const float fade = (kFadeSeconds < seconds) ? kFadeSeconds : seconds * 0.5f;

    const auto ns = std::chrono::nanoseconds(
        static_cast<long long>(static_cast<double>(seconds) * 1e9));

    std::lock_guard<std::mutex> lock(mutex_);
    text_ = text;
    expiry_ = Clock::now() + ns;
    fadeSeconds_ = fade;
    active_ = true;
}

void ToastState::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    active_ = false;
    text_.clear();
}

bool ToastState::Active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ && Clock::now() < expiry_;
}

bool ToastState::Snapshot(std::wstring& outText, float& outAlpha) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_) return false;

    const auto now = Clock::now();
    if (now >= expiry_) return false;

    const double remaining =
        std::chrono::duration_cast<std::chrono::duration<double>>(expiry_ - now).count();

    float alpha = 1.0f;
    if (fadeSeconds_ > 0.0f && remaining < static_cast<double>(fadeSeconds_))
        alpha = static_cast<float>(remaining / static_cast<double>(fadeSeconds_));
    if (alpha < 0.0f) alpha = 0.0f;
    else if (alpha > 1.0f) alpha = 1.0f;

    outText = text_;
    outAlpha = alpha;
    return true;
}

}  // namespace dxr

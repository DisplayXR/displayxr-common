// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Env-gated per-stage frame timing (runtime#837).
 *
 * Header-only, dependency-free. Splits a frame loop into named stages via
 * mark() calls, accumulates per-stage wall time, and emits a one-line summary
 * roughly once per second. This is the instrumentation that split the
 * avatar's 24.4 ms frame in minutes and found the two per-frame stalls that
 * gated 60 fps — wire it into every demo's loop behind its own env var so
 * the next regression is a log line, not a night of bisecting.
 *
 * Usage:
 * @code
 *   static dxr::FrameStageTimer s_ft("DXR_MYAPP_STAGE_TIMING",
 *       {"wait", "render", "layers", "end"});          // N stage names
 *   // per frame, N+1 marks bracketing the N stages:
 *   s_ft.mark(0);  BeginFrame(...);
 *   s_ft.mark(1);  RenderViews(...);
 *   s_ft.mark(2);  BuildLayers(...);
 *   s_ft.mark(3);  xrEndFrame(...);
 *   if (const char* line = s_ft.commitFrame()) LOG_INFO("%s", line);
 * @endcode
 *
 * commitFrame() also accumulates the gap between the previous frame's commit
 * and this frame's mark(0) as an implicit trailing "other" stage. Marks not
 * taken this frame (early-outs) skip the frame. Single-threaded.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

namespace dxr {

class FrameStageTimer {
public:
    static constexpr uint32_t kMaxStages = 12;

    FrameStageTimer(const char* envVar, std::initializer_list<const char*> stageNames)
    {
        const char* e = std::getenv(envVar);
        enabled_ = (e != nullptr && *e != '\0' && *e != '0');
        for (const char* n : stageNames) {
            if (names_.size() >= kMaxStages) break;
            names_.push_back(n);
        }
        accum_.assign(names_.size() + 1, 0.0); // +1: implicit "other"
        marks_.assign(names_.size() + 1, 0);
    }

    bool enabled() const { return enabled_; }

    //! Record boundary @p idx (0..N for N stages). mark(0) starts the frame.
    void
    mark(uint32_t idx)
    {
        if (!enabled_ || idx >= marks_.size()) return;
        if (idx == 0) {
            for (auto& m : marks_) m = 0;
        }
        marks_[idx] = now();
    }

    //! Fold this frame's marks into the accumulators. Returns a formatted
    //! summary line ~once per second (valid until the next call), else NULL.
    const char*
    commitFrame()
    {
        if (!enabled_) return nullptr;
        for (const uint64_t m : marks_) {
            if (m == 0) return nullptr; // incomplete frame (early-out path)
        }
        for (size_t i = 0; i + 1 < marks_.size(); ++i) {
            accum_[i] += double(marks_[i + 1] - marks_[i]) * 1e-6;
        }
        if (prevEnd_ != 0) {
            accum_[names_.size()] += double(marks_[0] - prevEnd_) * 1e-6;
        }
        prevEnd_ = marks_.back();
        frames_++;

        if (lastLog_ == 0) lastLog_ = prevEnd_;
        if (prevEnd_ - lastLog_ < 1000000000ull || frames_ == 0) return nullptr;

        int off = std::snprintf(line_, sizeof(line_), "[STAGES] n=%u", frames_);
        const double inv = 1.0 / frames_;
        for (size_t i = 0; i < names_.size() && off > 0 && off < (int)sizeof(line_); ++i) {
            off += std::snprintf(line_ + off, sizeof(line_) - off, " %s=%.2f", names_[i],
                                 accum_[i] * inv);
        }
        if (off > 0 && off < (int)sizeof(line_)) {
            std::snprintf(line_ + off, sizeof(line_) - off, " other=%.2f (ms/frame)",
                          accum_[names_.size()] * inv);
        }
        for (auto& a : accum_) a = 0.0;
        frames_ = 0;
        lastLog_ = prevEnd_;
        return line_;
    }

private:
    static uint64_t
    now()
    {
        return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    bool enabled_ = false;
    std::vector<const char*> names_;
    std::vector<double> accum_;   // per stage + trailing "other", ms
    std::vector<uint64_t> marks_; // boundary timestamps, ns
    uint32_t frames_ = 0;
    uint64_t lastLog_ = 0, prevEnd_ = 0;
    char line_[512] = {};
};

} // namespace dxr

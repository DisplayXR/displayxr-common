// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  GPU-free test for vk_frame_ring.h.
 *
 * Exercises the pure helpers the ring is built from (slot advance + which
 * fence a frame waits, env clamp, UBO slice alignment, deferred-deletion
 * order) on every runner. When the Vulkan headers are available it also
 * compile-checks the full VkFrameRing class and vk_overlay_kit.h — no device
 * is created, so nothing needs the loader at link time.
 */

#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

#if defined(__has_include)
#if __has_include(<vulkan/vulkan.h>)
#define DXR_TEST_HAS_VULKAN 1
#endif
#endif

#ifndef DXR_TEST_HAS_VULKAN
#define DXR_VK_FRAME_RING_PURE_ONLY 1
#endif
#include "vk_frame_ring.h"
#ifdef DXR_TEST_HAS_VULKAN
#include "vk_overlay_kit.h"
#endif

static int g_failures = 0;

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            ++g_failures;                                                                  \
        }                                                                                  \
    } while (0)

using namespace dxr::detail;

static void
testAlignUp()
{
    CHECK(alignUp(0, 256) == 0);
    CHECK(alignUp(1, 256) == 256);
    CHECK(alignUp(256, 256) == 256);
    CHECK(alignUp(257, 256) == 512);
    CHECK(alignUp(208, 64) == 256);
    CHECK(alignUp(208, 16) == 208);
    CHECK(alignUp(13, 0) == 13); // 0 / 1 = unaligned
    CHECK(alignUp(13, 1) == 13);
    CHECK(alignUp(10, 12) == 12); // non-power-of-two still rounds up
}

static void
testParseFramesInFlight()
{
    CHECK(parseFramesInFlight(nullptr, 0) == 3); // default
    CHECK(parseFramesInFlight("", 0) == 3);
    CHECK(parseFramesInFlight(nullptr, 2) == 2); // explicit config (Android path)
    CHECK(parseFramesInFlight(nullptr, 99) == 8); // config clamped
    CHECK(parseFramesInFlight("1", 3) == 1); // env wins: A/B the anti-pattern
    CHECK(parseFramesInFlight("4", 2) == 4);
    CHECK(parseFramesInFlight("8", 0) == 8);
    CHECK(parseFramesInFlight("9", 0) == 8); // clamp high
    CHECK(parseFramesInFlight("1000000", 0) == 8);
    CHECK(parseFramesInFlight("0", 2) == 2); // 0 = not set
    CHECK(parseFramesInFlight("-3", 0) == 3); // negative = not set
    CHECK(parseFramesInFlight("abc", 5) == 5); // garbage = not set
    CHECK(parseFramesInFlight("3x", 0) == 3); // trailing junk = not set
    CHECK(parseFramesInFlight("3x", 6) == 6);
    CHECK(clampFramesInFlight(0) == 3);
    CHECK(clampFramesInFlight(1) == 1);
    CHECK(clampFramesInFlight(9) == 8);
}

//! Model the ring exactly as VkFrameRing drives it: idx starts at n-1, each
//! beginFrame() advances then waits that slot's fence if pending, endFrame()
//! marks it pending. The fence waited at frame k must be frame k-n's — never
//! the frame just submitted (k-1) unless n == 1.
static void
testRingSchedule()
{
    for (uint32_t n = 1; n <= kMaxFramesInFlight; ++n) {
        uint32_t idx = n - 1;
        std::vector<int> submittedFrame(n, -1); // -1 = never submitted (not pending)
        for (int k = 0; k < 50; ++k) {
            idx = ringAdvance(idx, n);
            CHECK(idx == uint32_t(k) % n);
            const int waited = submittedFrame[idx]; // beginFrame waits this
            if (k < int(n)) {
                CHECK(waited == -1); // warm-up: nothing to wait
            } else {
                CHECK(waited == k - int(n));
                if (n >= 2) CHECK(waited != k - 1); // never the frame just submitted
            }
            submittedFrame[idx] = k; // endFrame
        }
    }
    CHECK(ringAdvance(0, 0) == 0); // degenerate, no division by zero
    CHECK(ringAdvance(2, 3) == 0);
}

static void
testUboSlices()
{
    // 3 slots x 4 views, 208-byte UBO at 256-byte alignment.
    const uint64_t stride = alignUp(208, 256);
    CHECK(stride == 256);
    std::vector<uint64_t> seen;
    for (uint32_t s = 0; s < 3; ++s) {
        for (uint32_t v = 0; v < 4; ++v) {
            const uint64_t off = uboSliceOffset(s, v, 4, stride);
            CHECK(off % 256 == 0);       // every dynamic offset is legal
            CHECK(off + 208 <= 3 * 4 * stride); // inside the buffer
            for (uint64_t o : seen) CHECK(o != off); // slots/views disjoint
            seen.push_back(off);
        }
    }
    CHECK(uboSliceOffset(0, 0, 4, stride) == 0);
    CHECK(uboSliceOffset(1, 0, 4, stride) == 4 * stride);
    CHECK(uboSliceOffset(2, 3, 4, stride) == 11 * stride);
}

static void
testDeferredQueue()
{
    DeferredQueue q;
    std::string log;
    CHECK(q.empty());
    q.push([&] { log += "a"; });
    q.push([&] { log += "b"; });
    q.push(nullptr); // ignored
    q.push([&] {
        log += "c";
        q.push([&] { log += "d"; }); // queued from inside: next run, not now
    });
    CHECK(q.size() == 3);
    q.run();
    CHECK(log == "abc"); // FIFO
    CHECK(q.size() == 1);
    q.run();
    CHECK(log == "abcd");
    CHECK(q.empty());
    q.run(); // empty run is a no-op
    CHECK(log == "abcd");
}

#ifdef DXR_TEST_HAS_VULKAN
static void
testFullHeaderCompiles()
{
    static_assert(!std::is_copy_constructible<dxr::VkFrameRing>::value, "ring owns handles");
    static_assert(!std::is_copy_assignable<dxr::VkFrameRing>::value, "ring owns handles");
    dxr::VkFrameRing::Config cfg;
    CHECK(cfg.framesInFlight == 0);
    CHECK(cfg.perViewUboBytes == 0);
    CHECK(cfg.maxViewsPerFrame == 16);
    // Signatures downstream code is written against. decltype is unevaluated,
    // so this checks the API without ODR-using (and so linking) any vk* call.
    using R = dxr::VkFrameRing;
    static_assert(std::is_same<decltype(&R::beginFrame), VkCommandBuffer (R::*)()>::value, "");
    static_assert(std::is_same<decltype(&R::current), VkCommandBuffer (R::*)() const>::value, "");
    static_assert(std::is_same<decltype(&R::endFrame),
                               VkResult (R::*)(const VkSemaphore*, const VkPipelineStageFlags*,
                                               uint32_t, const VkSemaphore*, uint32_t)>::value,
                  "");
    static_assert(std::is_same<decltype(&R::viewUbo), void* (R::*)(uint32_t, uint32_t*)>::value,
                  "");
    static_assert(std::is_same<decltype(&R::uboBuffer), VkBuffer (R::*)() const>::value, "");
    static_assert(std::is_same<decltype(&R::tileBarrier), void (*)(VkCommandBuffer)>::value, "");
    static_assert(
        std::is_same<decltype(&R::colorAttachmentBarrier), void (*)(VkCommandBuffer)>::value, "");
    static_assert(
        std::is_same<decltype(&R::deferDestroy), void (R::*)(std::function<void()>)>::value, "");
    static_assert(std::is_same<decltype(&R::waitAll), void (R::*)()>::value, "");
    static_assert(std::is_same<decltype(&R::submitImmediate),
                               VkResult (R::*)(const std::function<void(VkCommandBuffer)>&)>::value,
                  "");
    static_assert(std::is_same<decltype(&R::init),
                               bool (R::*)(VkDevice, VkPhysicalDevice, VkQueue, uint32_t,
                                           const R::Config&)>::value,
                  "");
    std::printf("vk_frame_ring_test: full header (Vulkan) compiled\n");
}
#endif

int
main()
{
    testAlignUp();
    testParseFramesInFlight();
    testRingSchedule();
    testUboSlices();
    testDeferredQueue();
#ifdef DXR_TEST_HAS_VULKAN
    testFullHeaderCompiles();
#else
    std::printf("vk_frame_ring_test: no Vulkan headers — pure helpers only\n");
#endif
    if (g_failures != 0) {
        std::fprintf(stderr, "vk_frame_ring_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("vk_frame_ring_test: OK\n");
    return 0;
}

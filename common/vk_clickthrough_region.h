// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Coverage-driven window-region punch-through for transparent VK
 *         overlay apps (runtime#833 / #837, Windows-only).
 *
 * Under decoupled presentation (DXR_PRESENT_OPAQUE) DWM completes no blends:
 * an un-shaped "transparent" window shows the ~1-frame-late WGC bake instead
 * of the live desktop. The one mechanism that punches through to the REAL
 * desktop is the window region — outside it the HWND doesn't exist, for
 * rendering or hit-testing (the Unity/avatar recipe). This helper derives
 * that region from the frame's OWN rendered view (no extra scene pass):
 *
 *   update(): downscale-blit the view image's alpha to a small coverage
 *   image, read it back through a fence-pipelined copy (consumed on the NEXT
 *   call — never a synchronous wait, the stall that cost the avatar 8
 *   ms/frame), dilate, build one RECT per horizontal run of covered texels,
 *   union any caller "chrome" rects (button bars, bubbles, toasts), and
 *   SetWindowRgn the result.
 *
 * THE REGION IS A VISUAL CLIP, NOT ONLY A HIT MASK. SetWindowRgn deletes
 * every pixel outside it — the window does not exist there, for rendering as
 * well as for input. This file used to justify its resolution cap and its
 * binary threshold against mouse hit-testing ("finer than the dilation
 * hit-tests apply anyway"), which was sound for input and was never
 * established for what the user sees. It was not true for the eye: at the old
 * fixed 256x144 raster a 3840 px window got 15 px per texel, each texel
 * binary, with no dilation at all, so thin features (fingers, a hat brim,
 * hair, a bubble tail) fell below one texel and vanished from the window.
 * Every trade-off below is now stated against the visual consumer:
 *
 *   - The raster SCALES with the window (kWindowPxPerTexel px per texel, up
 *     to kCovMaxW x kCovMaxH), so precision no longer degrades as the window
 *     grows. Affordable because the readback buffer is HOST_CACHED — see
 *     FindHostVisibleType in vk_overlay_kit.h; the caps were defending
 *     against a cost that was mostly an uncached read.
 *   - The coverage is DILATED by kDilateTexels before the rects are built.
 *     The error is asymmetric: an over-large region leaves a few transparent
 *     pixels clickable, an under-large one deletes content. Bias outward.
 *   - The region uses a LOW alpha threshold (regionAlpha(), default 8), not
 *     the hit test's 40. A high threshold keeps stray clicks off nearly
 *     transparent pixels; applied to a visual clip the same threshold erases
 *     anti-aliased and feathered edges.
 *
 * Rules learned the hard way (see runtime#837 / plugin#116 history):
 *   - Shape only BORDERLESS (WS_POPUP) windows. A shaped
 *     WS_EX_NOREDIRECTIONBITMAP window can never paint an OS frame, and
 *     style churn while shaped makes the flip-chain window vanish.
 *   - disable() (leaving transparent mode) un-shapes and resets so the next
 *     enable starts clean.
 *   - Both halves of the outermost-view union come from the SAME frame (one
 *     command buffer, two blits). Do not "optimise" that into alternating
 *     views: the halves then mix data frames apart and the region trails a
 *     fast-moving subject, clipping its leading edge.
 *   - The coverage lags ONE update() call — the pipelined readback, which is
 *     the stall the design correctly avoids. Do not add more.
 *
 * Env levers (all optional; defaults are the shipping values):
 *   DXR_CLICKTHROUGH_TEXEL_PX  window px per coverage texel (default 4)
 *   DXR_CLICKTHROUGH_DILATE    dilation radius in texels (default 1)
 *   DXR_CLICKTHROUGH_ALPHA     region alpha threshold 1..255 (default 8)
 *   DXR_CLICKTHROUGH_TIMING=1  per-stage ms + rect count, ~once a second
 *   DXR_CLICKTHROUGH_DUMP=1    ~1/s BMP of the mask to
 *                              %TEMP%\dxr_clickthrough_mask.bmp — white =
 *                              covered, grey = added by dilation, black =
 *                              clipped away. Diff it against the frame to
 *                              see exactly what the region is deleting.
 *   DXR_VK_NO_HOST_CACHED=1    force the legacy write-combined readback
 *
 * Usage (render thread, per frame while transparent mode is active):
 * @code
 *   static dxr::ClickThroughRegion s_punch;        // + init() once
 *   // after the frame's views rendered, BEFORE releasing the view image:
 *   RECT chrome[] = { barBandRectInClientPx };
 *   s_punch.update(queue, viewImage, viewW, viewH, hwnd, winW, winH,
 *                  chrome, 1);
 *   // on leaving transparent mode:
 *   s_punch.disable(hwnd);
 *   // WM_NCHITTEST: return HTCLIENT while shaped — the OS only delivers
 *   // hits inside the region, everything outside reaches the desktop.
 * @endcode
 */

#pragma once

#if defined(_WIN32)

#include <windows.h>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "frame_stage_timer.h" // dxr::FrameStageTimer
#include "vk_overlay_kit.h"    // dxr::detail::FencedStage

//! displayxr-common's file logger (logging.cpp). Forward-declared rather than
//! including logging.h: that header #defines UNICODE before <windows.h> and
//! this one must stay includable in any order.
void Log(const char* level, const char* format, ...);

namespace dxr {

class ClickThroughRegion {
public:
    //! Coverage-raster ceiling. The raster scales with the window and only
    //! stops here: 1024x576 holds 4 px/texel out to a 4096x2304 window, and
    //! the readback it implies (1024*576*4*2 = 4.7 MB) is a HOST_CACHED
    //! linear read — not the phantom cost the old 256x144 cap defended
    //! against.
    static constexpr uint32_t kCovMaxW = 1024;
    static constexpr uint32_t kCovMaxH = 576;
    //! Floor, so a tiny window still gets a usable silhouette.
    static constexpr uint32_t kCovMinW = 64;
    static constexpr uint32_t kCovMinH = 64;
    //! Window pixels per coverage texel. 4 px is roughly where the region's
    //! stair-stepping stops being visible along a silhouette edge.
    static constexpr uint32_t kWindowPxPerTexel = 4;
    //! Dilation radius, in coverage texels, applied before rects are built.
    //! One texel closes the gaps a thin feature leaves when the downscale
    //! blit's 2x2 taps catch it in only some rows (see update()).
    static constexpr uint32_t kDilateTexels = 1;
    //! Alpha above this counts as covered FOR A HIT TEST. Kept for callers
    //! that run their own WM_NCHITTEST sampling; the region itself uses the
    //! much lower regionAlpha(), because erasing a soft edge is a visual bug
    //! and letting a click land on a nearly transparent pixel is not.
    static constexpr uint8_t kAlphaThreshold = 40;
    static constexpr uint8_t kRegionAlphaDefault = 8;

    bool
    init(VkDevice dev, VkPhysicalDevice phys, uint32_t queueFamily)
    {
        dev_ = dev;
        // Two slices: view 0 and (optionally) the LAST view. The on-screen
        // weave occupies the UNION of the views' footprints — a single view's
        // silhouette clips the other views' parallax edges once the content
        // moves off ZDP (the gauss butterfly bug). Sized for the ceiling; a
        // smaller window uses a prefix of each slice.
        //
        // forCpuReads: this buffer is read back every frame. Without
        // HOST_CACHED it is write-combined on a discrete GPU and the read
        // costs two orders of magnitude more than the copy (vk_overlay_kit.h).
        if (!stage_.init(dev, phys, queueFamily, VkDeviceSize(kCovMaxW) * kCovMaxH * 4 * 2,
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT, /*forCpuReads=*/true)) {
            return false;
        }
        Log(stage_.hostCached ? "INFO" : "WARN",
            "[clickthrough] coverage readback is %s — CPU reads go %s",
            stage_.hostCached ? "HOST_CACHED" : "WRITE-COMBINED (no cached type)",
            stage_.hostCached ? "through the cache" : "uncached; the per-frame scan will be slow");
        // Small BLIT_DST + TRANSFER_SRC coverage image the view blits into.
        // Allocated at the ceiling and used as a sub-rect, so a window resize
        // never has to recreate it under an in-flight copy.
        VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {kCovMaxW, kCovMaxH, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(dev, &ici, nullptr, &covImage_) != VK_SUCCESS) return false;
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(dev, covImage_, &mr);
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(phys, &mp);
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
            if ((mr.memoryTypeBits & (1u << i)) &&
                (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                type = i;
                break;
            }
        }
        if (type == UINT32_MAX) return false;
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = type;
        if (vkAllocateMemory(dev, &ai, nullptr, &covMem_) != VK_SUCCESS ||
            vkBindImageMemory(dev, covImage_, covMem_, 0) != VK_SUCCESS) {
            return false;
        }
        return true;
    }

    void
    destroy()
    {
        stage_.destroy();
        if (covImage_ != VK_NULL_HANDLE) vkDestroyImage(dev_, covImage_, nullptr);
        if (covMem_ != VK_NULL_HANDLE) vkFreeMemory(dev_, covMem_, nullptr);
        covImage_ = VK_NULL_HANDLE;
        covMem_ = VK_NULL_HANDLE;
    }

    bool shaped() const { return shaped_; }

    //! Coverage raster the last applied region was built from (diagnostics).
    uint32_t coverageWidth() const { return lastCovW_; }
    uint32_t coverageHeight() const { return lastCovH_; }
    uint32_t rectCount() const { return lastRects_; }

    /*!
     * One punch-through step. Consumes the previous call's readback (fence
     * ~free by now) and applies the region; then blits @p viewImage
     * (COLOR_ATTACHMENT_OPTIMAL, restored on exit) down to the coverage
     * image and submits the copy for the NEXT call. @p chrome rects
     * (client px, may be NULL) stay in-region — visible AND clickable.
     * Call only while the window is borderless (WS_POPUP).
     */
    void
    update(VkQueue queue, VkImage viewImage, uint32_t viewW, uint32_t viewH, HWND hwnd,
           uint32_t winW, uint32_t winH, const RECT* chrome, uint32_t chromeCount,
           uint32_t lastViewX = 0, uint32_t lastViewY = 0, bool unionLastView = false)
    {
        if (covImage_ == VK_NULL_HANDLE || viewImage == VK_NULL_HANDLE || winW == 0 || winH == 0) {
            return;
        }

        // 1. Consume the previous readback -> shape the window.
        if (stage_.pending) {
            stage_.retire();
            const uint8_t* px = static_cast<const uint8_t*>(stage_.mapped);
            if (px != nullptr && pendCovW_ > 0 && pendCovH_ > 0) {
                const size_t slice = size_t(pendCovW_) * pendCovH_ * 4;
                applyRegion(px, pendTwo_ ? px + slice : nullptr, hwnd, pendCovW_, pendCovH_,
                            pendWinW_, pendWinH_, chrome, chromeCount);
            }
        } else {
            stage_.retire(); // free any stale cmd
        }

        // 2. Kick this frame's blit + copy (no wait).
        //
        // Coverage resolution tracks the window instead of sitting at a fixed
        // cap. The single LINEAR blit still takes only 2x2 taps per texel, so
        // at a large reduction ratio a sub-texel feature is caught in some rows
        // and missed in others; kWindowPxPerTexel keeps that ratio small and
        // the dilation in applyRegion() closes what is left. If a case ever
        // still drops isolated thin features, the fix is a box/mip reduction
        // here — not a lower alpha threshold, which cannot recover a feature
        // the filter never sampled.
        const uint32_t covW = covDim(winW, kCovMinW, kCovMaxW);
        const uint32_t covH = covDim(winH, kCovMinH, kCovMaxH);

        VkCommandBuffer cmd = stage_.beginCmd();
        if (cmd == VK_NULL_HANDLE) return;

        VkImageMemoryBarrier bar = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        // View -> TRANSFER_SRC (and back after the blit: the runtime consumes
        // the released swapchain image in COLOR_ATTACHMENT).
        bar.image = viewImage;
        bar.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        bar.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);

        bar.image = covImage_;
        bar.srcAccessMask = 0;
        bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &bar);

        VkImageBlit blit = {};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {(int32_t)viewW, (int32_t)viewH, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[1] = {(int32_t)covW, (int32_t)covH, 1};
        vkCmdBlitImage(cmd, viewImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, covImage_,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

        // Slice 0: view 0's coverage.
        bar.image = covImage_;
        bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &bar);

        VkBufferImageCopy rg = {};
        rg.bufferRowLength = covW;
        rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rg.imageExtent = {covW, covH, 1};
        vkCmdCopyImageToBuffer(cmd, covImage_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stage_.buf, 1,
                               &rg);

        if (unionLastView) {
            // Slice 1: the LAST view's tile -> second buffer slice, in THIS
            // command buffer. Both halves of the union must come from the same
            // frame; alternating them makes the region trail moving content.
            bar.image = covImage_;
            bar.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &bar);
            VkImageBlit blit2 = {};
            blit2.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit2.srcOffsets[0] = {(int32_t)lastViewX, (int32_t)lastViewY, 0};
            blit2.srcOffsets[1] = {(int32_t)(lastViewX + viewW), (int32_t)(lastViewY + viewH), 1};
            blit2.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit2.dstOffsets[1] = {(int32_t)covW, (int32_t)covH, 1};
            vkCmdBlitImage(cmd, viewImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, covImage_,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit2, VK_FILTER_LINEAR);
            bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &bar);
            VkBufferImageCopy rg2 = rg;
            rg2.bufferOffset = VkDeviceSize(covW) * covH * 4;
            vkCmdCopyImageToBuffer(cmd, covImage_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stage_.buf,
                                   1, &rg2);
        }

        // Restore the view image for the runtime's consumption.
        bar.image = viewImage;
        bar.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        bar.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        bar.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &bar);
        vkEndCommandBuffer(cmd);

        if (stage_.submit(queue, cmd)) {
            pendWinW_ = winW;
            pendWinH_ = winH;
            pendCovW_ = covW;
            pendCovH_ = covH;
            pendTwo_ = unionLastView;
        }
    }

    //! Leave punch-through mode: wait out any in-flight copy and un-shape.
    void
    disable(HWND hwnd)
    {
        stage_.retire();
        lastSig_ = 0; // the OS no longer holds our region — never skip the next apply
        if (shaped_) {
            SetWindowRgn(hwnd, nullptr, TRUE);
            shaped_ = false;
        }
    }

private:
    //! Window px -> coverage texels, scaled and clamped.
    static uint32_t
    covDim(uint32_t windowPx, uint32_t lo, uint32_t hi)
    {
        const uint32_t per = texelPx();
        uint32_t n = (windowPx + per - 1) / per;
        if (n < lo) n = lo;
        if (n > hi) n = hi;
        return n;
    }

    static uint32_t
    envUInt(const char* name, uint32_t def, uint32_t lo, uint32_t hi)
    {
        const char* e = std::getenv(name);
        if (e == nullptr || e[0] == 0) return def;
        const long v = std::strtol(e, nullptr, 10);
        if (v < (long)lo || v > (long)hi) return def;
        return (uint32_t)v;
    }

    static uint32_t
    texelPx()
    {
        static const uint32_t v = envUInt("DXR_CLICKTHROUGH_TEXEL_PX", kWindowPxPerTexel, 1, 64);
        return v;
    }
    static uint32_t
    dilateTexels()
    {
        static const uint32_t v = envUInt("DXR_CLICKTHROUGH_DILATE", kDilateTexels, 0, 16);
        return v;
    }
    static uint8_t
    regionAlpha()
    {
        static const uint8_t v =
            (uint8_t)envUInt("DXR_CLICKTHROUGH_ALPHA", kRegionAlphaDefault, 1, 255);
        return v;
    }
    static bool
    dumpEnabled()
    {
        static const bool v = envUInt("DXR_CLICKTHROUGH_DUMP", 0, 0, 1) != 0;
        return v;
    }

    void
    applyRegion(const uint8_t* px, const uint8_t* px2, HWND hwnd, uint32_t covW, uint32_t covH,
                uint32_t winW, uint32_t winH, const RECT* chrome, uint32_t chromeCount)
    {
        if (winW == 0 || winH == 0 || covW == 0 || covH == 0) return;

        static FrameStageTimer s_ft("DXR_CLICKTHROUGH_TIMING",
                                    {"read", "dilate", "rects", "setrgn"});
        s_ft.mark(0);

        const size_t n = size_t(covW) * covH;
        const uint8_t alphaMin = regionAlpha();

        // Pull each row out of the mapped buffer with memcpy BEFORE touching it
        // byte-wise. memcpy uses wide streaming loads, which survive
        // write-combined memory; the per-texel loop below does not. Cheap
        // insurance for drivers that expose no HOST_CACHED type.
        cov_.assign(n, 0);
        row_.resize(size_t(covW) * 4);
        for (uint32_t y = 0; y < covH; ++y) {
            uint8_t* out = cov_.data() + size_t(y) * covW;
            std::memcpy(row_.data(), px + size_t(y) * covW * 4, size_t(covW) * 4);
            for (uint32_t x = 0; x < covW; ++x) {
                if (row_[size_t(x) * 4 + 3] > alphaMin) out[x] = 1;
            }
            if (px2 != nullptr) {
                std::memcpy(row_.data(), px2 + size_t(y) * covW * 4, size_t(covW) * 4);
                for (uint32_t x = 0; x < covW; ++x) {
                    if (row_[size_t(x) * 4 + 3] > alphaMin) out[x] = 1;
                }
            }
        }
        s_ft.mark(1);

        // Dilate (separable, R texels each way). An over-large region costs a
        // few transparent pixels that stay clickable; an under-large one
        // deletes content the user can see.
        //
        // Horizontal is run-based (a covered run just widens by R, so it is one
        // memset per run, not per texel). Vertical GATHERS 2R+1 source rows into
        // one destination row, in 64-bit words: the obvious scatter (OR each
        // source row into 2R+1 destinations) does 2R+1 times the writes and was
        // measured at ~2.9 ms/frame on a 792x428 raster.
        const uint32_t R = dilateTexels();
        const uint8_t* mask = cov_.data();
        if (R > 0) {
            const size_t stride = (size_t(covW) + 7u) & ~size_t(7u); // 8-byte aligned rows
            dilA_.assign(stride * covH, 0);
            for (uint32_t y = 0; y < covH; ++y) {
                const uint8_t* src = cov_.data() + size_t(y) * covW;
                uint8_t* dst = dilA_.data() + size_t(y) * stride;
                uint32_t x = 0;
                while (x < covW) {
                    if (!src[x]) {
                        ++x;
                        continue;
                    }
                    const uint32_t xs = x;
                    while (x < covW && src[x]) ++x;
                    const uint32_t x0 = (xs > R) ? xs - R : 0;
                    const uint32_t x1 = (x + R < covW) ? x + R : covW;
                    std::memset(dst + x0, 1, x1 - x0);
                }
            }
            dilB_.assign(stride * covH, 0);
            const size_t words = stride / 8;
            for (uint32_t y = 0; y < covH; ++y) {
                const uint32_t y0 = (y > R) ? y - R : 0;
                const uint32_t y1 = (y + R + 1 < covH) ? y + R + 1 : covH;
                uint64_t* dst = reinterpret_cast<uint64_t*>(dilB_.data() + size_t(y) * stride);
                const uint64_t* src0 =
                    reinterpret_cast<const uint64_t*>(dilA_.data() + size_t(y0) * stride);
                std::memcpy(dst, src0, stride);
                for (uint32_t yy = y0 + 1; yy < y1; ++yy) {
                    const uint64_t* s =
                        reinterpret_cast<const uint64_t*>(dilA_.data() + size_t(yy) * stride);
                    for (size_t w = 0; w < words; ++w) dst[w] |= s[w];
                }
            }
            mask = dilB_.data();
            maskStride_ = stride;
        } else {
            maskStride_ = covW;
        }
        s_ft.mark(2);

        // One RECT per horizontal run, with identical consecutive rows folded
        // into a single band — a 540-row raster otherwise hands GDI thousands
        // of rects a frame for a shape that is mostly vertical edges.
        rects_.clear();
        prevRuns_.clear();
        size_t bandFirst = SIZE_MAX;
        for (uint32_t y = 0; y < covH; ++y) {
            const uint8_t* rowMask = mask + size_t(y) * maskStride_;
            runs_.clear();
            uint32_t x = 0;
            while (x < covW) {
                if (!rowMask[x]) {
                    ++x;
                    continue;
                }
                const uint32_t xs = x;
                while (x < covW && rowMask[x]) ++x;
                runs_.push_back(xs);
                runs_.push_back(x);
            }
            const LONG bottom = (LONG)((uint64_t)(y + 1) * winH / covH);
            if (runs_.empty()) {
                bandFirst = SIZE_MAX;
                prevRuns_.clear();
                continue;
            }
            if (bandFirst != SIZE_MAX && runs_ == prevRuns_) {
                for (size_t i = bandFirst; i < rects_.size(); ++i) rects_[i].bottom = bottom;
                continue;
            }
            bandFirst = rects_.size();
            const LONG top = (LONG)((uint64_t)y * winH / covH);
            for (size_t i = 0; i + 1 < runs_.size(); i += 2) {
                RECT r;
                r.left = (LONG)((uint64_t)runs_[i] * winW / covW);
                r.right = (LONG)((uint64_t)runs_[i + 1] * winW / covW);
                r.top = top;
                r.bottom = bottom;
                rects_.push_back(r);
            }
            prevRuns_ = runs_;
        }
        for (uint32_t i = 0; i < chromeCount; ++i) {
            if (chrome[i].right > chrome[i].left && chrome[i].bottom > chrome[i].top) {
                rects_.push_back(chrome[i]);
            }
        }
        s_ft.mark(3);

        if (dumpEnabled()) dumpMask(cov_.data(), mask, maskStride_, covW, covH);

        // Nothing moved -> the OS already has exactly this region. SetWindowRgn
        // is a DWM re-shape, not a cheap setter (measured 1.5-4.5 ms/frame here,
        // the single largest stage), and a still avatar or a paused model hands
        // it the identical rect list every frame.
        const uint64_t sig = HashBytes(rects_.data(), rects_.size() * sizeof(RECT),
                                       0xcbf29ce484222325ull ^ rects_.size());
        if (shaped_ && sig == lastSig_) {
            lastCovW_ = covW;
            lastCovH_ = covH;
            lastRects_ = (uint32_t)rects_.size();
            s_ft.mark(4);
            if (const char* line = s_ft.commitFrame()) {
                Log("INFO", "[clickthrough] %s cov=%ux%u win=%ux%u rects=%u cached=%d", line, covW,
                    covH, winW, winH, lastRects_, (int)stage_.hostCached);
            }
            return;
        }

        HRGN rgn;
        if (rects_.empty()) {
            rgn = CreateRectRgn(0, 0, 0, 0); // fully click-through
        } else {
            const size_t bytes = sizeof(RGNDATAHEADER) + rects_.size() * sizeof(RECT);
            buf_.resize(bytes);
            RGNDATA* rd = reinterpret_cast<RGNDATA*>(buf_.data());
            rd->rdh.dwSize = sizeof(RGNDATAHEADER);
            rd->rdh.iType = RDH_RECTANGLES;
            rd->rdh.nCount = (DWORD)rects_.size();
            rd->rdh.nRgnSize = (DWORD)(rects_.size() * sizeof(RECT));
            RECT bb = rects_[0];
            for (const RECT& r : rects_) {
                if (r.left < bb.left) bb.left = r.left;
                if (r.top < bb.top) bb.top = r.top;
                if (r.right > bb.right) bb.right = r.right;
                if (r.bottom > bb.bottom) bb.bottom = r.bottom;
            }
            rd->rdh.rcBound = bb;
            std::memcpy(rd->Buffer, rects_.data(), rects_.size() * sizeof(RECT));
            rgn = ExtCreateRegion(nullptr, (DWORD)bytes, rd);
            if (rgn == nullptr) return;
        }
        SetWindowRgn(hwnd, rgn, TRUE); // OS owns rgn
        shaped_ = true;
        lastSig_ = sig;
        lastCovW_ = covW;
        lastCovH_ = covH;
        lastRects_ = (uint32_t)rects_.size();
        s_ft.mark(4);
        if (const char* line = s_ft.commitFrame()) {
            Log("INFO", "[clickthrough] %s cov=%ux%u win=%ux%u rects=%u cached=%d", line, covW,
                covH, winW, winH, lastRects_, (int)stage_.hostCached);
        }
    }

    /*!
     * ~1/s greyscale BMP of the mask to %TEMP%\dxr_clickthrough_mask.bmp:
     * white = covered by the raw alpha test, grey = added by the dilation,
     * black = clipped away. Diffing this against the rendered frame is what
     * turns "the region eats content" from an eyeballing exercise into a
     * comparison; the old code had no equivalent.
     */
    static void
    dumpMask(const uint8_t* raw, const uint8_t* dil, size_t dilStride, uint32_t w, uint32_t h)
    {
        static uint32_t s_n = 0;
        if ((s_n++ % 60) != 0) return;
        char tmp[MAX_PATH] = {0};
        if (GetTempPathA(MAX_PATH, tmp) == 0) return;
        char path[MAX_PATH * 2];
        std::snprintf(path, sizeof(path), "%sdxr_clickthrough_mask.bmp", tmp);
        FILE* f = std::fopen(path, "wb");
        if (f == nullptr) return;
        const uint32_t rowBytes = ((w * 3u) + 3u) & ~3u;
        const uint32_t imgBytes = rowBytes * h;
        const uint32_t fileBytes = 54 + imgBytes;
        const uint32_t pixOffset = 54;
        uint8_t fh[14] = {'B', 'M'};
        std::memcpy(fh + 2, &fileBytes, 4);
        std::memcpy(fh + 10, &pixOffset, 4);
        std::fwrite(fh, 1, 14, f);
        uint8_t ih[40] = {0};
        const uint32_t hdrSize = 40;
        const int32_t iw = (int32_t)w, ihh = (int32_t)h;
        const uint16_t planes = 1, bpp = 24;
        std::memcpy(ih + 0, &hdrSize, 4);
        std::memcpy(ih + 4, &iw, 4);
        std::memcpy(ih + 8, &ihh, 4);
        std::memcpy(ih + 12, &planes, 2);
        std::memcpy(ih + 14, &bpp, 2);
        std::memcpy(ih + 20, &imgBytes, 4);
        std::fwrite(ih, 1, 40, f);
        std::vector<uint8_t> row(rowBytes, 0);
        for (int32_t y = (int32_t)h - 1; y >= 0; --y) { // BMP rows are bottom-up
            const uint8_t* r = raw + size_t(y) * w;
            const uint8_t* d = dil + size_t(y) * dilStride;
            for (uint32_t x = 0; x < w; ++x) {
                const uint8_t v = r[x] ? 255 : (d[x] ? 160 : 0);
                row[x * 3 + 0] = v;
                row[x * 3 + 1] = v;
                row[x * 3 + 2] = v;
            }
            std::fwrite(row.data(), 1, rowBytes, f);
        }
        std::fclose(f);
    }

    VkDevice dev_ = VK_NULL_HANDLE;
    detail::FencedStage stage_;
    VkImage covImage_ = VK_NULL_HANDLE;
    VkDeviceMemory covMem_ = VK_NULL_HANDLE;
    uint32_t pendWinW_ = 0, pendWinH_ = 0;
    uint32_t pendCovW_ = 0, pendCovH_ = 0;
    bool pendTwo_ = false;
    bool shaped_ = false;
    uint32_t lastCovW_ = 0, lastCovH_ = 0, lastRects_ = 0;
    uint64_t lastSig_ = 0;   // rect-list hash of the region the OS currently holds
    size_t maskStride_ = 0;  // row stride of `mask` (padded when dilating)
    // Scratch, reused across frames: this runs every frame on the render
    // thread and a 540-row raster is not something to reallocate 60x a second.
    std::vector<uint8_t> cov_, dilA_, dilB_, row_, buf_;
    std::vector<RECT> rects_;
    std::vector<uint32_t> runs_, prevRuns_;
};

} // namespace dxr

#endif // _WIN32

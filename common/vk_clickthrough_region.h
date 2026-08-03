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
 *   image (≤256×144), read it back through a fence-pipelined copy (consumed
 *   on the NEXT call — never a synchronous wait, the stall that cost the
 *   avatar 8 ms/frame), build one RECT per horizontal run of covered pixels,
 *   union any caller "chrome" rects (button bars, bubbles, toasts), and
 *   SetWindowRgn the result.
 *
 * Rules learned the hard way (see runtime#837 / plugin#116 history):
 *   - Shape only BORDERLESS (WS_POPUP) windows. A shaped
 *     WS_EX_NOREDIRECTIONBITMAP window can never paint an OS frame, and
 *     style churn while shaped makes the flip-chain window vanish.
 *   - disable() (leaving transparent mode) un-shapes and resets so the next
 *     enable starts clean.
 *   - The coverage lags ~2 update() calls — invisible for hit masks and
 *     silhouette punch-through under the standard dilation.
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
#include <cstring>
#include <vector>

#include "vk_overlay_kit.h" // dxr::detail::FencedStage

namespace dxr {

class ClickThroughRegion {
public:
    //! Coverage raster is capped at kCovW×kCovH — ~3 px hit precision on an
    //! 800 px window, finer than the dilation hit-tests apply anyway.
    static constexpr uint32_t kCovW = 256;
    static constexpr uint32_t kCovH = 144;
    //! Alpha above this counts as covered (matches the avatar's threshold).
    static constexpr uint8_t kAlphaThreshold = 40;

    bool
    init(VkDevice dev, VkPhysicalDevice phys, uint32_t queueFamily)
    {
        dev_ = dev;
        // Two slices: view 0 and (optionally) the LAST view. The on-screen
        // weave occupies the UNION of the views' footprints — a single view's
        // silhouette clips the other views' parallax edges once the content
        // moves off ZDP (the gauss butterfly bug).
        if (!stage_.init(dev, phys, queueFamily, VkDeviceSize(kCovW) * kCovH * 4 * 2,
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
            return false;
        }
        // Small BLIT_DST + TRANSFER_SRC coverage image the view blits into.
        VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {kCovW, kCovH, 1};
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

        // 1. Consume the previous readback → shape the window.
        if (stage_.pending) {
            stage_.retire();
            const uint8_t* px = static_cast<const uint8_t*>(stage_.mapped);
            if (px != nullptr) {
                applyRegion(px, pendTwo_ ? px + size_t(kCovW) * kCovH * 4 : nullptr, hwnd,
                            pendWinW_, pendWinH_, chrome, chromeCount);
            }
        } else {
            stage_.retire(); // free any stale cmd
        }

        // 2. Kick this frame's blit + copy (no wait).
        VkCommandBuffer cmd = stage_.beginCmd();
        if (cmd == VK_NULL_HANDLE) return;

        VkImageMemoryBarrier bar = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        // View → TRANSFER_SRC (and back after the blit: the runtime consumes
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
        blit.dstOffsets[1] = {(int32_t)kCovW, (int32_t)kCovH, 1};
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
        rg.bufferRowLength = kCovW;
        rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rg.imageExtent = {kCovW, kCovH, 1};
        vkCmdCopyImageToBuffer(cmd, covImage_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stage_.buf, 1,
                               &rg);

        if (unionLastView) {
            // Slice 1: the LAST view's tile → second buffer slice. The view
            // image is still TRANSFER_SRC (restored below).
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
            blit2.dstOffsets[1] = {(int32_t)kCovW, (int32_t)kCovH, 1};
            vkCmdBlitImage(cmd, viewImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, covImage_,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit2, VK_FILTER_LINEAR);
            bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &bar);
            VkBufferImageCopy rg2 = rg;
            rg2.bufferOffset = VkDeviceSize(kCovW) * kCovH * 4;
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
            pendTwo_ = unionLastView;
        }
    }

    //! Leave punch-through mode: wait out any in-flight copy and un-shape.
    void
    disable(HWND hwnd)
    {
        stage_.retire();
        if (shaped_) {
            SetWindowRgn(hwnd, nullptr, TRUE);
            shaped_ = false;
        }
    }

private:
    void
    applyRegion(const uint8_t* px, const uint8_t* px2, HWND hwnd, uint32_t winW, uint32_t winH,
                const RECT* chrome, uint32_t chromeCount)
    {
        if (winW == 0 || winH == 0) return;
        auto covered = [&](uint32_t x, uint32_t y) {
            const size_t i = (size_t(y) * kCovW + x) * 4 + 3;
            if (px[i] > kAlphaThreshold) return true;
            return px2 != nullptr && px2[i] > kAlphaThreshold;
        };
        std::vector<RECT> rects;
        rects.reserve(kCovH + chromeCount);
        for (uint32_t y = 0; y < kCovH; ++y) {
            uint32_t x = 0;
            while (x < kCovW) {
                if (!covered(x, y)) {
                    ++x;
                    continue;
                }
                const uint32_t xs = x;
                while (x < kCovW && covered(x, y)) ++x;
                RECT r;
                r.left = (LONG)(xs * winW / kCovW);
                r.right = (LONG)(x * winW / kCovW);
                r.top = (LONG)(y * winH / kCovH);
                r.bottom = (LONG)((y + 1) * winH / kCovH);
                rects.push_back(r);
            }
        }
        for (uint32_t i = 0; i < chromeCount; ++i) {
            if (chrome[i].right > chrome[i].left && chrome[i].bottom > chrome[i].top) {
                rects.push_back(chrome[i]);
            }
        }

        HRGN rgn;
        if (rects.empty()) {
            rgn = CreateRectRgn(0, 0, 0, 0); // fully click-through
        } else {
            const size_t bytes = sizeof(RGNDATAHEADER) + rects.size() * sizeof(RECT);
            std::vector<uint8_t> buf(bytes);
            RGNDATA* rd = reinterpret_cast<RGNDATA*>(buf.data());
            rd->rdh.dwSize = sizeof(RGNDATAHEADER);
            rd->rdh.iType = RDH_RECTANGLES;
            rd->rdh.nCount = (DWORD)rects.size();
            rd->rdh.nRgnSize = (DWORD)(rects.size() * sizeof(RECT));
            RECT bb = rects[0];
            for (const RECT& r : rects) {
                if (r.left < bb.left) bb.left = r.left;
                if (r.top < bb.top) bb.top = r.top;
                if (r.right > bb.right) bb.right = r.right;
                if (r.bottom > bb.bottom) bb.bottom = r.bottom;
            }
            rd->rdh.rcBound = bb;
            std::memcpy(rd->Buffer, rects.data(), rects.size() * sizeof(RECT));
            rgn = ExtCreateRegion(nullptr, (DWORD)bytes, rd);
            if (rgn == nullptr) return;
        }
        SetWindowRgn(hwnd, rgn, TRUE); // OS owns rgn
        shaped_ = true;
    }

    VkDevice dev_ = VK_NULL_HANDLE;
    detail::FencedStage stage_;
    VkImage covImage_ = VK_NULL_HANDLE;
    VkDeviceMemory covMem_ = VK_NULL_HANDLE;
    uint32_t pendWinW_ = 0, pendWinH_ = 0;
    bool pendTwo_ = false;
    bool shaped_ = false;
};

} // namespace dxr

#endif // _WIN32

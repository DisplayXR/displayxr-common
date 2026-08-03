// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vulkan transparent-overlay kit: cached layer uploads + pipelined
 *         GPU→CPU readbacks (runtime#837).
 *
 * Header-only (displayxr-common links no Vulkan; only VK apps include this).
 * Both classes exist to kill one anti-pattern that cost the avatar ~16 ms of
 * every iGPU frame and exists copy-pasted across the demos: per-frame
 * "render → staging upload → vkQueueSubmit → vkQueueWaitIdle" for content
 * that either never changes (a static HUD/bubble panel) or whose consumer
 * tolerates a couple frames of lag (a click-through coverage mask). On the
 * single queue of an Intel iGPU each such wait drains the *whole frame's*
 * queued GPU work into the app thread.
 *
 * The shared discipline: submit with a fence, never wait on the hot path —
 * consume/reuse on the NEXT invocation, by which time the fence is long
 * signaled (the runtime's per-frame commit already drains the queue).
 *
 *   dxr::CachedLayerUploader  — window-space / Local2D layer image uploads,
 *       gated on a caller-provided content hash: unchanged hash = complete
 *       no-op (don't even acquire the swapchain image); the last-released
 *       image keeps serving the layer. Changed hash = staging copy submitted
 *       with a fence, no wait.
 *   dxr::PipelinedReadback    — image→host readback whose result is consumed
 *       one invocation later (the avatar silhouette pattern, 41→60 fps).
 *   dxr::HashBytes            — FNV-1a helper for content hashes (hash the
 *       source pixels, or cheaper: the strings/geometry that produced them).
 *
 * Usage (cached layer, replaces the per-frame upload+wait):
 * @code
 *   static dxr::CachedLayerUploader s_hud;                 // + init() once
 *   uint64_t h = dxr::HashBytes(text.data(), text.size() * sizeof(wchar_t),
 *                               dxr::HashBytes(&geom, sizeof(geom)));
 *   if (s_hud.needsUpload(h)) {
 *       const void* px = RenderHudToTexture(...);          // rasterize ONLY now
 *       uint32_t idx;
 *       if (px && AcquireHudSwapchainImage(xr, idx)) {
 *           s_hud.upload(queue, images[idx], px, pitch, texW, texH, h);
 *           ReleaseHudSwapchainImage(xr);
 *       }
 *   }
 *   // Submit the layer struct every frame regardless — the last-released
 *   // swapchain image persists.
 * @endcode
 *
 * Usage (pipelined readback, replaces the synchronous fence wait):
 * @code
 *   static dxr::PipelinedReadback s_rb;                    // + init() once
 *   s_rb.consumePending([&](const uint8_t* px, uint32_t w, uint32_t h,
 *                           uint32_t tag) { ...use px... });  // ~free wait
 *   RenderCoverage(...);                                   // app render pass
 *   s_rb.submitCopy(queue, coverageImage, w, h, slot);     // fence, no wait
 * @endcode
 *
 * Threading: single-threaded per instance (call from the render thread).
 * Cleanup: destroy() waits the in-flight fence, then frees everything.
 */

#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstring>
#include <functional>

namespace dxr {

//! FNV-1a over @p n bytes; chain via @p seed. Never returns 0 (0 = "no
//! content yet" sentinel in CachedLayerUploader).
inline uint64_t
HashBytes(const void* p, size_t n, uint64_t seed = 1469598103934665603ull)
{
    const uint8_t* b = static_cast<const uint8_t*>(p);
    uint64_t h = seed;
    for (size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h == 0 ? 1 : h;
}

namespace detail {

inline uint32_t
FindHostVisibleType(VkPhysicalDevice phys, uint32_t typeBits)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    const VkMemoryPropertyFlags want =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) {
            return i;
        }
    }
    return UINT32_MAX;
}

//! Shared plumbing: a mapped host-visible buffer + own cmd pool + one fence +
//! the lazily-retired previous command buffer.
struct FencedStage {
    VkDevice dev = VK_NULL_HANDLE;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize bytes = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkCommandBuffer prevCmd = VK_NULL_HANDLE;
    bool pending = false;

    bool
    init(VkDevice device, VkPhysicalDevice phys, uint32_t queueFamily, VkDeviceSize size,
         VkBufferUsageFlags usage)
    {
        dev = device;
        bytes = size;
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = size;
        bi.usage = usage;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(dev, &bi, nullptr, &buf) != VK_SUCCESS) return false;
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(dev, buf, &mr);
        const uint32_t type = FindHostVisibleType(phys, mr.memoryTypeBits);
        if (type == UINT32_MAX) return false;
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = type;
        if (vkAllocateMemory(dev, &ai, nullptr, &mem) != VK_SUCCESS ||
            vkBindBufferMemory(dev, buf, mem, 0) != VK_SUCCESS ||
            vkMapMemory(dev, mem, 0, size, 0, &mapped) != VK_SUCCESS) {
            return false;
        }
        VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = queueFamily;
        if (vkCreateCommandPool(dev, &pi, nullptr, &pool) != VK_SUCCESS) return false;
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        return vkCreateFence(dev, &fi, nullptr, &fence) == VK_SUCCESS;
    }

    //! Wait the in-flight submission (if any) and retire its command buffer.
    //! Called at the START of the next use — by then the fence is long
    //! signaled, so the wait is ~free.
    void
    retire()
    {
        if (pending) {
            vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
            pending = false;
        }
        if (prevCmd != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(dev, pool, 1, &prevCmd);
            prevCmd = VK_NULL_HANDLE;
        }
    }

    //! Submit @p cmd on @p queue with the fence; the cmd retires on the next
    //! retire() call. Returns false (and frees the cmd) on submit failure.
    bool
    submit(VkQueue queue, VkCommandBuffer cmd)
    {
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vkResetFences(dev, 1, &fence);
        if (vkQueueSubmit(queue, 1, &si, fence) != VK_SUCCESS) {
            vkFreeCommandBuffers(dev, pool, 1, &cmd);
            return false;
        }
        pending = true;
        prevCmd = cmd;
        return true;
    }

    VkCommandBuffer
    beginCmd()
    {
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(dev, &ai, &cmd) != VK_SUCCESS) return VK_NULL_HANDLE;
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
            vkFreeCommandBuffers(dev, pool, 1, &cmd);
            return VK_NULL_HANDLE;
        }
        return cmd;
    }

    void
    destroy()
    {
        if (dev == VK_NULL_HANDLE) return;
        retire();
        if (fence != VK_NULL_HANDLE) vkDestroyFence(dev, fence, nullptr);
        if (pool != VK_NULL_HANDLE) vkDestroyCommandPool(dev, pool, nullptr);
        if (mem != VK_NULL_HANDLE) {
            if (mapped != nullptr) vkUnmapMemory(dev, mem);
            vkFreeMemory(dev, mem, nullptr);
        }
        if (buf != VK_NULL_HANDLE) vkDestroyBuffer(dev, buf, nullptr);
        *this = FencedStage{};
    }
};

} // namespace detail

/*!
 * Content-hash-gated uploader for a layer swapchain image (RGBA8). Unchanged
 * hash → needsUpload() false → the caller skips rasterization AND the
 * swapchain acquire entirely; the last-released image keeps serving the
 * submitted layer. Changed hash → one staging copy submitted with a fence and
 * NO wait. Barriers match the demos' hand-rolled pattern (UNDEFINED →
 * TRANSFER_DST → COLOR_ATTACHMENT_OPTIMAL).
 */
class CachedLayerUploader {
public:
    //! @p texW/@p texH: the layer texture's allocated dimensions (the staging
    //! buffer is sized texW*texH*4 once).
    bool
    init(VkDevice dev, VkPhysicalDevice phys, uint32_t queueFamily, uint32_t texW, uint32_t texH)
    {
        texW_ = texW;
        texH_ = texH;
        return stage_.init(dev, phys, queueFamily, VkDeviceSize(texW) * texH * 4,
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    }

    void destroy() { stage_.destroy(); }

    //! True when @p contentHash differs from the last successful upload.
    bool needsUpload(uint64_t contentHash) const { return contentHash != lastHash_; }

    /*!
     * Copy @p pixels (@p srcPitch bytes/row, @p rows rows of @p rowBytes) into
     * staging and submit the buffer→image copy. No wait: the previous upload
     * (if any) is retired first (~free — it is frames old). Marks the hash on
     * successful submission.
     */
    bool
    upload(VkQueue queue, VkImage image, const void* pixels, uint32_t srcPitch, uint32_t rowBytes,
           uint32_t rows, uint64_t contentHash)
    {
        if (stage_.mapped == nullptr || pixels == nullptr || rows > texH_ || rowBytes > texW_ * 4) {
            return false;
        }
        stage_.retire();

        uint8_t* dst = static_cast<uint8_t*>(stage_.mapped);
        const uint8_t* src = static_cast<const uint8_t*>(pixels);
        for (uint32_t r = 0; r < rows; ++r) {
            std::memcpy(dst + size_t(r) * texW_ * 4, src + size_t(r) * srcPitch, rowBytes);
        }

        VkCommandBuffer cmd = stage_.beginCmd();
        if (cmd == VK_NULL_HANDLE) return false;

        VkImageMemoryBarrier bar = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        bar.srcAccessMask = 0;
        bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = image;
        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &bar);

        VkBufferImageCopy rg = {};
        rg.bufferRowLength = texW_;
        rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rg.imageExtent = {texW_, rows, 1};
        vkCmdCopyBufferToImage(cmd, stage_.buf, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);

        bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &bar);
        vkEndCommandBuffer(cmd);

        if (!stage_.submit(queue, cmd)) return false;
        lastHash_ = contentHash;
        return true;
    }

    //! Force the next needsUpload() true (e.g. after a swapchain recreate).
    void invalidate() { lastHash_ = 0; }

private:
    detail::FencedStage stage_;
    uint32_t texW_ = 0, texH_ = 0;
    uint64_t lastHash_ = 0; // 0 = nothing uploaded yet (HashBytes never returns 0)
};

/*!
 * Fence-pipelined image→host readback: submitCopy() records the copy and
 * submits with a fence, no wait; the NEXT consumePending() waits the fence
 * (~free by then) and hands the mapped pixels to the callback. Dropping a
 * pending result on dimension change is the caller's business via the
 * dimensions echoed to the callback.
 */
class PipelinedReadback {
public:
    bool
    init(VkDevice dev, VkPhysicalDevice phys, uint32_t queueFamily, VkDeviceSize maxBytes)
    {
        return stage_.init(dev, phys, queueFamily, maxBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    }

    void destroy() { stage_.destroy(); }

    //! If a copy is in flight: wait its fence, invoke @p consume(pixels, w, h,
    //! tag) with the mapped RGBA8 data, and clear the pending state. Returns
    //! whether @p consume ran. Call BEFORE re-rendering the source image and
    //! BEFORE any resize that recreates it.
    bool
    consumePending(const std::function<void(const uint8_t*, uint32_t, uint32_t, uint32_t)>& consume)
    {
        if (!stage_.pending) return false;
        stage_.retire();
        if (stage_.mapped == nullptr) return false;
        consume(static_cast<const uint8_t*>(stage_.mapped), pendW_, pendH_, pendTag_);
        return true;
    }

    //! Record @p image (COLOR_ATTACHMENT_OPTIMAL, w×h RGBA8) → staging copy and
    //! submit with the fence; no wait. @p tag is echoed to the consumer
    //! (e.g. a view slot). The image is left in TRANSFER_SRC_OPTIMAL — pass
    //! the next render's oldLayout accordingly, or re-transition yourself.
    bool
    submitCopy(VkQueue queue, VkImage image, uint32_t w, uint32_t h, uint32_t tag)
    {
        if (VkDeviceSize(w) * h * 4 > stage_.bytes) return false;
        stage_.retire();

        VkCommandBuffer cmd = stage_.beginCmd();
        if (cmd == VK_NULL_HANDLE) return false;

        VkImageMemoryBarrier bar = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        bar.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        bar.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = image;
        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);

        VkBufferImageCopy rg = {};
        rg.bufferRowLength = w;
        rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rg.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stage_.buf, 1, &rg);
        vkEndCommandBuffer(cmd);

        if (!stage_.submit(queue, cmd)) return false;
        pendW_ = w;
        pendH_ = h;
        pendTag_ = tag;
        return true;
    }

private:
    detail::FencedStage stage_;
    uint32_t pendW_ = 0, pendH_ = 0, pendTag_ = 0;
};

} // namespace dxr

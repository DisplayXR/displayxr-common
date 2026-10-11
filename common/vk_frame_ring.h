// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vulkan frame ring: N frames in flight, ONE vkQueueSubmit per frame,
 *         never a wait on the frame just submitted.
 *
 * Header-only (displayxr-common links no Vulkan; only VK apps include this —
 * same convention as vk_overlay_kit.h: the loader-exported vk* symbols are
 * called directly, so the consumer's existing Vulkan link satisfies them).
 *
 * ## Why
 *
 * Mobile GPU governors (Adreno's `msm-adreno-tz` is the measured case) pick
 * the clock from GPU *busy %*. An app that submits, waits the fence, then
 * submits again — per frame, or worse per TILE — leaves the GPU idle while
 * the CPU waits, so the governor reads ~50 % busy and parks the GPU at about a
 * third of its clock, which makes every frame slower, which keeps it idle.
 * displayxr-demo-modelviewer went 13 → 30 fps on an Adreno 740 (PR #170) by
 * keeping 3 frames in flight with one submit per frame; this header is the
 * generic half of that fix.
 *
 * ## The rule
 *
 *   1. Keep >= 2 frames in flight (default 3).
 *   2. ONE vkQueueSubmit per frame — all tiles/views of the atlas go into one
 *      command buffer, ordered by a barrier between tiles, not by a CPU wait.
 *   3. Never wait the fence of the frame you just submitted. The only wait on
 *      the hot path is beginFrame() waiting the fence of the frame that used
 *      the same slot N frames ago — normally long signaled.
 *   4. Never allocate command buffers per frame — the ring allocates N once and
 *      vkResetCommandBuffer()s them (per-frame vkAllocateCommandBuffers costs
 *      the Adreno driver a fresh zeroed allocation every time; runtime #1936).
 *
 * ## Usage skeleton (OpenXR projection layer, one atlas swapchain)
 * @code
 *   dxr::VkFrameRing ring;
 *   dxr::VkFrameRing::Config cfg;
 *   cfg.perViewUboBytes = sizeof(MyViewUbo);   // 0 = no UBO
 *   cfg.maxViewsPerFrame = kMaxViews;
 *   ring.init(device, physDevice, queue, queueFamily, cfg);
 *   // one descriptor set, binding = UNIFORM_BUFFER_DYNAMIC on ring.uboBuffer(),
 *   // range = ring.uboSliceBytes()
 *
 *   // per frame:
 *   xrWaitFrame / xrBeginFrame / xrLocateViews
 *   xrAcquireSwapchainImage + xrWaitSwapchainImage
 *   VkCommandBuffer cmd = ring.beginFrame();
 *   for (uint32_t i = 0; i < viewCount; ++i) {
 *       if (i > 0) dxr::VkFrameRing::tileBarrier(cmd);  // shared targets
 *       uint32_t dynOff;
 *       auto* ubo = static_cast<MyViewUbo*>(ring.viewUbo(i, &dynOff));
 *       *ubo = ...;                                        // slot-private
 *       vkCmdBindDescriptorSets(cmd, ..., 1, &dynOff);
 *       ...record tile i into its atlas sub-rect...
 *   }
 *   VkResult r = ring.endFrame();          // ONE submit, no wait
 *   if (r != VK_SUCCESS) log(r);
 *   xrReleaseSwapchainImage                // AFTER endFrame
 *   xrEndFrame
 * @endcode
 *
 * ## Do NOT
 *
 *   - vkQueueWaitIdle / vkDeviceWaitIdle / vkWaitForFences on the frame you
 *     just submitted, anywhere on the per-frame path.
 *   - Submit once per tile or per eye (with or without a wait in between).
 *   - vkAllocateCommandBuffers / vkFreeCommandBuffers per frame or per upload.
 *   - Write a host-visible buffer that an in-flight frame may still read —
 *     give each slot its own copy (viewUbo()), or waitAll() first.
 *   - Destroy a resource the GPU may still be reading — deferDestroy() it.
 *   - Release the swapchain image before endFrame(): the runtime composes on
 *     the same VkQueue, so the app's work must be queued ahead of it.
 *   - Block anywhere except submitImmediate() (the one named escape hatch for
 *     genuine one-off uploads) and waitAll() (resize / teardown).
 *
 * The frames-in-flight count can be overridden with the env var
 * DXR_VK_FRAMES_IN_FLIGHT (clamped 1..8) for A/B tests — 1 reproduces the
 * submit→wait anti-pattern. Android processes have no usable environment;
 * Android callers pass Config::framesInFlight explicitly.
 *
 * Threading: single-threaded per instance (call from the render thread). The
 * queue is the caller's; external synchronisation of it is the caller's too.
 *
 * Compile note: defining DXR_VK_FRAME_RING_PURE_ONLY before including this
 * header compiles ONLY the Vulkan-free helpers in dxr::detail (used by the
 * unit test on runners without the Vulkan SDK).
 */

#pragma once

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <utility>
#include <vector>

namespace dxr {
namespace detail {

//! Default frames in flight (modelviewer #170's measured value).
constexpr uint32_t kDefaultFramesInFlight = 3;
//! Upper clamp for frames in flight.
constexpr uint32_t kMaxFramesInFlight = 8;

//! Round @p v up to a multiple of @p align (0 or 1 = no alignment; any other
//! value need not be a power of two, though Vulkan's always are).
inline uint64_t
alignUp(uint64_t v, uint64_t align)
{
    if (align <= 1) return v;
    return ((v + align - 1) / align) * align;
}

//! Clamp a frames-in-flight count to [1, kMaxFramesInFlight]; 0 = default.
inline uint32_t
clampFramesInFlight(uint32_t n)
{
    if (n == 0) return kDefaultFramesInFlight;
    return n > kMaxFramesInFlight ? kMaxFramesInFlight : n;
}

/*!
 * Resolve the frames-in-flight count. @p env is the DXR_VK_FRAMES_IN_FLIGHT
 * value (may be null): a decimal integer wins over @p configured, clamped to
 * [1, 8]. Null / empty / non-numeric / 0 falls back to @p configured (itself
 * 0 = default 3, else clamped).
 */
inline uint32_t
parseFramesInFlight(const char* env, uint32_t configured)
{
    if (env != nullptr && env[0] != 0) {
        char* end = nullptr;
        const long v = std::strtol(env, &end, 10);
        if (end != env && *end == 0 && v > 0) {
            return v > long(kMaxFramesInFlight) ? kMaxFramesInFlight : uint32_t(v);
        }
    }
    return clampFramesInFlight(configured);
}

//! Next slot index in an @p n-slot ring.
inline uint32_t
ringAdvance(uint32_t idx, uint32_t n)
{
    return n == 0 ? 0 : (idx + 1) % n;
}

//! Byte offset of view @p view's slice in slot @p slot, in a UBO laid out as
//! slots × maxViews slices of @p stride bytes (stride already aligned).
inline uint64_t
uboSliceOffset(uint32_t slot, uint32_t view, uint32_t maxViews, uint64_t stride)
{
    return (uint64_t(slot) * maxViews + view) * stride;
}

//! FIFO of deferred actions; run() drains in registration order. Actions
//! queued from inside an action run on the NEXT run(), never re-entrantly.
class DeferredQueue {
public:
    void push(std::function<void()> fn)
    {
        if (fn) q_.push_back(std::move(fn));
    }
    void run()
    {
        std::vector<std::function<void()>> todo;
        todo.swap(q_);
        for (auto& fn : todo) fn();
    }
    size_t size() const { return q_.size(); }
    bool empty() const { return q_.empty(); }

private:
    std::vector<std::function<void()>> q_;
};

} // namespace detail
} // namespace dxr

#ifndef DXR_VK_FRAME_RING_PURE_ONLY

#include <vulkan/vulkan.h>

namespace dxr {

/*!
 * N-slot command-buffer / fence / UBO ring. See the file comment for the rule
 * and the usage skeleton.
 */
class VkFrameRing {
public:
    struct Config {
        //! Frames in flight; 0 = default (3). DXR_VK_FRAMES_IN_FLIGHT, when
        //! set, overrides this (A/B knob). Clamped to 1..8.
        uint32_t framesInFlight = 0;
        //! Bytes of one view's UBO slice; 0 = the ring owns no UBO.
        VkDeviceSize perViewUboBytes = 0;
        //! UBO slices per slot (views/tiles per frame). Ignored without a UBO.
        uint32_t maxViewsPerFrame = 16;
    };

    VkFrameRing() = default;
    VkFrameRing(const VkFrameRing&) = delete;
    VkFrameRing& operator=(const VkFrameRing&) = delete;
    ~VkFrameRing() { destroy(); }

    //! Create the pool, N command buffers, N fences and the optional UBO, all
    //! ONCE. Returns false (and frees anything created) on failure.
    bool
    init(VkDevice device, VkPhysicalDevice phys, VkQueue queue, uint32_t queueFamily,
         const Config& cfg)
    {
        destroy();
        dev_ = device;
        queue_ = queue;
        n_ = detail::parseFramesInFlight(std::getenv("DXR_VK_FRAMES_IN_FLIGHT"),
                                         cfg.framesInFlight);
        slots_.assign(n_, Slot{});
        idx_ = n_ - 1; // first beginFrame() lands on slot 0

        VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = queueFamily;
        if (vkCreateCommandPool(dev_, &pi, nullptr, &pool_) != VK_SUCCESS) return fail();

        // N frame command buffers + the submitImmediate() one, in one call.
        std::vector<VkCommandBuffer> cmds(n_ + 1, VK_NULL_HANDLE);
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = n_ + 1;
        if (vkAllocateCommandBuffers(dev_, &ai, cmds.data()) != VK_SUCCESS) return fail();
        for (uint32_t i = 0; i < n_; ++i) slots_[i].cmd = cmds[i];
        immCmd_ = cmds[n_];

        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        for (Slot& s : slots_) {
            if (vkCreateFence(dev_, &fi, nullptr, &s.fence) != VK_SUCCESS) return fail();
        }
        if (vkCreateFence(dev_, &fi, nullptr, &immFence_) != VK_SUCCESS) return fail();

        if (cfg.perViewUboBytes > 0 && cfg.maxViewsPerFrame > 0) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(phys, &props);
            uboSlice_ = cfg.perViewUboBytes;
            uboStride_ = detail::alignUp(cfg.perViewUboBytes,
                                         props.limits.minUniformBufferOffsetAlignment);
            maxViews_ = cfg.maxViewsPerFrame;
            if (!createUbo(phys, uboStride_ * maxViews_ * n_)) return fail();
        }
        inited_ = true;
        return true;
    }

    /*!
     * Advance to the next slot, wait ONLY that slot's fence (the frame that
     * used it N frames ago) if still pending, run its deferred deletions,
     * reset and begin its command buffer (ONE_TIME_SUBMIT). Returns the
     * recording command buffer, or VK_NULL_HANDLE on failure. Calling it while
     * a frame is already open returns the open frame's buffer unchanged.
     */
    VkCommandBuffer
    beginFrame()
    {
        if (!inited_) return VK_NULL_HANDLE;
        if (open_) return slots_[idx_].cmd;
        idx_ = detail::ringAdvance(idx_, n_);
        Slot& s = slots_[idx_];
        waitSlot(s);
        s.deferred.run();
        vkResetCommandBuffer(s.cmd, 0);
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(s.cmd, &bi) != VK_SUCCESS) return VK_NULL_HANDLE;
        open_ = true;
        return s.cmd;
    }

    //! The open frame's command buffer (VK_NULL_HANDLE outside begin/end).
    VkCommandBuffer current() const { return open_ ? slots_[idx_].cmd : VK_NULL_HANDLE; }

    /*!
     * This slot's private UBO slice for @p view (host-coherent, persistently
     * mapped — write it, no flush needed). @p outDynamicOffset receives the
     * dynamic offset to pass to vkCmdBindDescriptorSets for a
     * UNIFORM_BUFFER_DYNAMIC binding on uboBuffer(). Returns null outside an
     * open frame, without a UBO, or when @p view >= maxViewsPerFrame.
     */
    void*
    viewUbo(uint32_t view, uint32_t* outDynamicOffset)
    {
        if (!open_ || uboMapped_ == nullptr || view >= maxViews_) return nullptr;
        const uint64_t off = detail::uboSliceOffset(idx_, view, maxViews_, uboStride_);
        if (outDynamicOffset != nullptr) *outDynamicOffset = uint32_t(off);
        return static_cast<uint8_t*>(uboMapped_) + off;
    }

    //! The ring's single UBO buffer (all slots × views; slots are disjoint
    //! ranges, so ONE descriptor set with a dynamic offset serves every slot).
    VkBuffer uboBuffer() const { return ubo_; }
    //! Descriptor range for the UBO binding (= Config::perViewUboBytes).
    VkDeviceSize uboSliceBytes() const { return uboSlice_; }
    //! Aligned distance between consecutive slices.
    VkDeviceSize uboStride() const { return uboStride_; }

    uint32_t framesInFlight() const { return n_; }
    uint32_t slotIndex() const { return idx_; }
    bool frameOpen() const { return open_; }
    bool initialized() const { return inited_; }

    /*!
     * Full memory barrier, ALL_COMMANDS → ALL_COMMANDS (MEMORY_WRITE →
     * MEMORY_READ|MEMORY_WRITE). Record it between tiles that share any
     * intermediate target (colour / depth / MSAA / copy) — it is exactly the
     * ordering the old per-tile CPU wait gave, without the CPU in the loop.
     */
    static void
    tileBarrier(VkCommandBuffer cmd)
    {
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0,
                             nullptr);
    }

    /*!
     * Narrower variant: colour-attachment writes → fragment-shader reads,
     * transfer reads/writes, or the next tile's colour-attachment writes. Use
     * it only when the sole shared hazard is a colour target; shared DEPTH
     * targets or compute need tileBarrier().
     */
    static void
    colorAttachmentBarrier(VkCommandBuffer cmd)
    {
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                           VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                 VK_PIPELINE_STAGE_TRANSFER_BIT |
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             0, 1, &mb, 0, nullptr, 0, nullptr);
    }

    /*!
     * vkEndCommandBuffer + EXACTLY ONE vkQueueSubmit with the slot fence, and
     * NO wait. Call BEFORE xrReleaseSwapchainImage. A begun frame is always
     * submitted, even if nothing was recorded (an empty submit is cheap and
     * keeps the fence bookkeeping uniform). Returns the submit's VkResult for
     * the caller to log; on failure the slot is not marked pending (waiting a
     * fence that was never submitted would hang). VK_NOT_READY if no frame is
     * open.
     */
    VkResult
    endFrame(const VkSemaphore* waitSems = nullptr, const VkPipelineStageFlags* waitStages = nullptr,
             uint32_t waitCount = 0, const VkSemaphore* signalSems = nullptr,
             uint32_t signalCount = 0)
    {
        if (!open_) return VK_NOT_READY;
        Slot& s = slots_[idx_];
        open_ = false;
        VkResult r = vkEndCommandBuffer(s.cmd);
        if (r != VK_SUCCESS) return r;
        vkResetFences(dev_, 1, &s.fence);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = waitCount;
        si.pWaitSemaphores = waitSems;
        si.pWaitDstStageMask = waitStages;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &s.cmd;
        si.signalSemaphoreCount = signalCount;
        si.pSignalSemaphores = signalSems;
        r = vkQueueSubmit(queue_, 1, &si, s.fence);
        s.pending = (r == VK_SUCCESS);
        return r;
    }

    /*!
     * Run @p fn once the GPU has finished every frame submitted so far — i.e.
     * when the current slot's fence is next waited (N frames from now, or at
     * waitAll()/destroy()). Use for buffers/images/descriptor sets an
     * in-flight frame may still read. Valid inside or outside a frame.
     */
    void
    deferDestroy(std::function<void()> fn)
    {
        if (!inited_) {
            if (fn) fn();
            return;
        }
        slots_[idx_].deferred.push(std::move(fn));
    }

    /*!
     * Wait every pending fence and run all deferred deletions. For resize,
     * teardown, or a host rewrite of a buffer shared by all frames. Blocks —
     * never call it per frame. If a frame is open, its deferred list is left
     * alone (the open command buffer may still reference those resources).
     */
    void
    waitAll()
    {
        if (!inited_) return;
        for (uint32_t i = 0; i < n_; ++i) {
            Slot& s = slots_[i];
            waitSlot(s);
            if (!(open_ && i == idx_)) s.deferred.run();
        }
    }

    /*!
     * The ONLY sanctioned blocking submit: record a genuine one-off (an asset
     * upload, a layout init) into the ring's dedicated transient command
     * buffer (allocated once, reset per use), submit it alone, and wait its
     * own fence. Never call it per frame. Independent of the open frame.
     */
    VkResult
    submitImmediate(const std::function<void(VkCommandBuffer)>& record)
    {
        if (!inited_) return VK_ERROR_INITIALIZATION_FAILED;
        vkResetCommandBuffer(immCmd_, 0);
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VkResult r = vkBeginCommandBuffer(immCmd_, &bi);
        if (r != VK_SUCCESS) return r;
        if (record) record(immCmd_);
        r = vkEndCommandBuffer(immCmd_);
        if (r != VK_SUCCESS) return r;
        vkResetFences(dev_, 1, &immFence_);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &immCmd_;
        r = vkQueueSubmit(queue_, 1, &si, immFence_);
        if (r != VK_SUCCESS) return r;
        return vkWaitForFences(dev_, 1, &immFence_, VK_TRUE, UINT64_MAX);
    }

    //! waitAll(), then free everything. Safe to call twice / uninitialised.
    void
    destroy()
    {
        if (dev_ == VK_NULL_HANDLE) return;
        if (open_) {
            // Close the recording so the pool can be destroyed cleanly; it is
            // never submitted.
            vkEndCommandBuffer(slots_[idx_].cmd);
            open_ = false;
        }
        waitAll();
        for (Slot& s : slots_) {
            s.deferred.run();
            if (s.fence != VK_NULL_HANDLE) vkDestroyFence(dev_, s.fence, nullptr);
        }
        if (immFence_ != VK_NULL_HANDLE) vkDestroyFence(dev_, immFence_, nullptr);
        if (pool_ != VK_NULL_HANDLE) vkDestroyCommandPool(dev_, pool_, nullptr); // frees cmds
        if (uboMem_ != VK_NULL_HANDLE) {
            if (uboMapped_ != nullptr) vkUnmapMemory(dev_, uboMem_);
            vkFreeMemory(dev_, uboMem_, nullptr);
        }
        if (ubo_ != VK_NULL_HANDLE) vkDestroyBuffer(dev_, ubo_, nullptr);
        slots_.clear();
        dev_ = VK_NULL_HANDLE;
        queue_ = VK_NULL_HANDLE;
        pool_ = VK_NULL_HANDLE;
        immCmd_ = VK_NULL_HANDLE;
        immFence_ = VK_NULL_HANDLE;
        ubo_ = VK_NULL_HANDLE;
        uboMem_ = VK_NULL_HANDLE;
        uboMapped_ = nullptr;
        uboSlice_ = uboStride_ = 0;
        maxViews_ = n_ = idx_ = 0;
        open_ = inited_ = false;
    }

private:
    struct Slot {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        bool pending = false; // submitted, fence not yet observed
        detail::DeferredQueue deferred;
    };

    void
    waitSlot(Slot& s)
    {
        if (s.pending) {
            vkWaitForFences(dev_, 1, &s.fence, VK_TRUE, UINT64_MAX);
            s.pending = false;
        }
    }

    bool
    fail()
    {
        inited_ = true; // let destroy() walk what was created
        destroy();
        return false;
    }

    bool
    createUbo(VkPhysicalDevice phys, VkDeviceSize bytes)
    {
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = bytes;
        bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(dev_, &bi, nullptr, &ubo_) != VK_SUCCESS) return false;
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(dev_, ubo_, &mr);
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(phys, &mp);
        const VkMemoryPropertyFlags host =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        // Prefer device-local host-visible (unified memory on mobile, BAR on
        // desktop), else any host-visible coherent type.
        uint32_t type = UINT32_MAX;
        const VkMemoryPropertyFlags wants[2] = {host | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, host};
        for (VkMemoryPropertyFlags want : wants) {
            for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; ++i) {
                if ((mr.memoryTypeBits & (1u << i)) &&
                    (mp.memoryTypes[i].propertyFlags & want) == want) {
                    type = i;
                }
            }
            if (type != UINT32_MAX) break;
        }
        if (type == UINT32_MAX) return false;
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = type;
        return vkAllocateMemory(dev_, &ai, nullptr, &uboMem_) == VK_SUCCESS &&
               vkBindBufferMemory(dev_, ubo_, uboMem_, 0) == VK_SUCCESS &&
               vkMapMemory(dev_, uboMem_, 0, VK_WHOLE_SIZE, 0, &uboMapped_) == VK_SUCCESS;
    }

    VkDevice dev_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    std::vector<Slot> slots_;
    uint32_t n_ = 0;
    uint32_t idx_ = 0;
    bool open_ = false;
    bool inited_ = false;

    VkCommandBuffer immCmd_ = VK_NULL_HANDLE;
    VkFence immFence_ = VK_NULL_HANDLE;

    VkBuffer ubo_ = VK_NULL_HANDLE;
    VkDeviceMemory uboMem_ = VK_NULL_HANDLE;
    void* uboMapped_ = nullptr;
    VkDeviceSize uboSlice_ = 0;
    VkDeviceSize uboStride_ = 0;
    uint32_t maxViews_ = 0;
};

} // namespace dxr

#endif // DXR_VK_FRAME_RING_PURE_ONLY

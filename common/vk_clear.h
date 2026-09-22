// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Display-referred clears for Vulkan (runtime #1647).
 *
 * Header-only (displayxr-common links no Vulkan; only VK apps include this).
 * The rule and the reasoning live in `clear_policy.h` — read that first. The
 * one line worth repeating here:
 *
 *   the question is what space the content written into this target is in, and
 *   the target's format answers it ONLY when that target is the thing that
 *   encodes; where a later blit or resolve does the encoding, the caller must
 *   say.
 *
 * Vulkan takes the clear value in the attachment's own space everywhere it
 * takes one — `VkRenderPassBeginInfo::pClearValues`, `vkCmdClearColorImage`,
 * `vkCmdClearAttachments`, and dynamic rendering's
 * `VkRenderingAttachmentInfo::clearValue` — so one helper covers all four.
 *
 * ── Which overload ──────────────────────────────────────────────────────────
 *
 * RENDERING STRAIGHT INTO THE SWAPCHAIN IMAGE (the attachment is what encodes):
 * pass the attachment's `VkFormat`.
 *
 * @code
 *   VkClearValue clears[2];
 *   clears[0].color = dxr::VkDisplayReferredClearColor(colorFormat_, kBackground);
 *   clears[1].depthStencil = {1.0f, 0};
 * @endcode
 *
 * RENDERING INTO AN INTERNAL IMAGE AND BLITTING INTO THE SWAPCHAIN: the
 * attachment's format does not decide. If the pipeline's shaders emit
 * scene-linear into that attachment — which they do whenever the blit's
 * destination is `_SRGB` and the blit is relied on for the encode — the clear
 * is scene-linear too, whatever the attachment's own format says. Say so:
 *
 * @code
 *   // colorFormat_ is R8G8B8A8_UNORM but the content is linear when the
 *   // swapchain is _SRGB, because vkCmdBlitImage does the encode.
 *   clears[0].color = dxr::VkDisplayReferredClearColor(
 *       swapchainIsSrgb_ ? dxr::ClearValueSpace::SceneLinear
 *                        : dxr::ClearValueSpace::DisplayReferred,
 *       kBackground);
 * @endcode
 *
 * Getting this wrong in the second shape is what the format-only rule missed:
 * it leaves the clear unconverted and the blit then encodes it, which is the
 * brightening this helper exists to stop.
 *
 * (Better still, where you can: pick the internal format FROM the swapchain
 * format so the pair never half-converts — see README § *If you blit into the
 * swapchain (Vulkan)*. Then the first overload is correct again.)
 *
 * Alpha is never converted. An unclassified format is cleared with the
 * authored value and warned about once.
 */

#pragma once

#include <vulkan/vulkan.h>

#include "clear_policy.h"

namespace dxr {

//! Clear colour for a target whose space the caller states outright.
inline VkClearColorValue
VkDisplayReferredClearColor(ClearValueSpace space, const float displayReferredRGBA[4])
{
    VkClearColorValue out = {};
    ApplyClearValueSpace(space, displayReferredRGBA, out.float32);
    return out;
}

//! Clear colour derived from the attachment's own `VkFormat`. Correct when the
//! attachment is what encodes; see the file comment for when it is not.
inline VkClearColorValue
VkDisplayReferredClearColor(VkFormat attachmentFormat, const float displayReferredRGBA[4])
{
    const ClearValueSpace space = VulkanClearValueSpace((int64_t)attachmentFormat);
    if (space == ClearValueSpace::Unknown) {
        ReportUnknownClearTarget("Vulkan", (long long)attachmentFormat);
    }
    return VkDisplayReferredClearColor(space, displayReferredRGBA);
}

//! `VkClearValue` convenience for a colour attachment slot.
inline VkClearValue
VkDisplayReferredClearValue(VkFormat attachmentFormat, const float displayReferredRGBA[4])
{
    VkClearValue out = {};
    out.color = VkDisplayReferredClearColor(attachmentFormat, displayReferredRGBA);
    return out;
}

//! @overload
inline VkClearValue
VkDisplayReferredClearValue(ClearValueSpace space, const float displayReferredRGBA[4])
{
    VkClearValue out = {};
    out.color = VkDisplayReferredClearColor(space, displayReferredRGBA);
    return out;
}

} // namespace dxr

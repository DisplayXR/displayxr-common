// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#include "xr_window_space_hud.h"
#include "color_policy.h" // ChooseWindowSpaceSwapchainFormat (ADR-044 §7)

#include <stdio.h>
#include <vector>

bool CreateHudSwapchain(XrSession session, uint32_t width, uint32_t height, XrHudSwapchain& out)
{
    uint32_t formatCount = 0;
    if (XR_FAILED(xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr)) || formatCount == 0) {
        return false;
    }
    std::vector<int64_t> formats(formatCount);
    if (XR_FAILED(xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data()))) {
        return false;
    }

    // ADR-044 §7 / INV-4.6: the R8G8B8A8 `_SRGB` sibling by default — the
    // same rule as xr_session_common.cpp::CreateWindowSpaceSwapchain, via the
    // one shared chooser. The CPU rasterizers (HudRenderer / HudRendererMacOS)
    // emit DISPLAY-REFERRED R8G8B8A8 bytes; on a format-honest runtime a UNORM
    // swapchain is read as LINEAR and encoded a second time (washed out), so
    // `_SRGB` is the honest declaration: DXGI 29, VK 43, GL_SRGB8_ALPHA8,
    // MTLPixelFormatRGBA8Unorm_sRGB (71). The family stays R8G8B8A8 so the
    // upload (CopyTextureRegion / vkCmdCopyBufferToImage / glTexSubImage2D /
    // replaceRegion:) stays a legal, byte-exact RAW copy. Anything that
    // renders, blits or clears into the image now encodes on write — move the
    // bytes with a raw copy only (README "Window-space swapchains").
    // DXR_SWAPCHAIN_ENCODING=unorm restores the R8G8B8A8 UNORM choice.
    const dxr::ColorFormatChoice choice = dxr::ChooseWindowSpaceSwapchainFormat(
        formats, dxr::ColorEncodingPreferenceFromEnvironment());
    const int64_t selected = choice.format;
    if (selected == 0) {
        return false;
    }

    XrSwapchainCreateInfo sci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                     XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                     XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sci.format = selected;
    sci.sampleCount = 1;
    sci.width = width;
    sci.height = height;
    sci.faceCount = 1;
    sci.arraySize = 1;
    sci.mipCount = 1;

    if (XR_FAILED(xrCreateSwapchain(session, &sci, &out.swapchain))) {
        return false;
    }
    out.format = selected;
    out.width = width;
    out.height = height;

    uint32_t imageCount = 0;
    if (XR_FAILED(xrEnumerateSwapchainImages(out.swapchain, 0, &imageCount, nullptr))) {
        return false;
    }
    out.imageCount = imageCount;
    return true;
}

bool AcquireHudSwapchainImage(const XrHudSwapchain& sc, uint32_t& outIndex)
{
    if (sc.swapchain == XR_NULL_HANDLE) return false;
    XrSwapchainImageAcquireInfo acq = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (XR_FAILED(xrAcquireSwapchainImage(sc.swapchain, &acq, &outIndex))) return false;
    XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wait.timeout = XR_INFINITE_DURATION;
    return XR_SUCCEEDED(xrWaitSwapchainImage(sc.swapchain, &wait));
}

bool ReleaseHudSwapchainImage(const XrHudSwapchain& sc)
{
    if (sc.swapchain == XR_NULL_HANDLE) return false;
    XrSwapchainImageReleaseInfo rel = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    return XR_SUCCEEDED(xrReleaseSwapchainImage(sc.swapchain, &rel));
}

bool SubmitWindowSpaceHudFrame(
    XrSession session,
    XrSpace localSpace,
    XrTime displayTime,
    XrEnvironmentBlendMode envBlendMode,
    const XrCompositionLayerProjectionView* projViews,
    uint32_t viewCount,
    const XrHudSwapchain& hud,
    float x, float y, float width, float height,
    float disparity,
    int32_t srcX, int32_t srcY,
    int32_t srcW, int32_t srcH)
{
    if (srcW < 0) srcW = (int32_t)hud.width;
    if (srcH < 0) srcH = (int32_t)hud.height;

    XrCompositionLayerProjection projLayer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    projLayer.space = localSpace;
    projLayer.viewCount = viewCount;
    projLayer.views = projViews;

    XrCompositionLayerWindowSpaceDXR hudLayer = {};
    hudLayer.type = (XrStructureType)XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_DXR;
    hudLayer.next = nullptr;
    hudLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    hudLayer.subImage.swapchain = hud.swapchain;
    hudLayer.subImage.imageRect.offset = { srcX, srcY };
    hudLayer.subImage.imageRect.extent = { srcW, srcH };
    hudLayer.subImage.imageArrayIndex = 0;
    hudLayer.x = x;
    hudLayer.y = y;
    hudLayer.width = width;
    hudLayer.height = height;
    hudLayer.disparity = disparity;

    const XrCompositionLayerBaseHeader* layers[] = {
        (XrCompositionLayerBaseHeader*)&projLayer,
        (XrCompositionLayerBaseHeader*)&hudLayer,
    };

    XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
    endInfo.displayTime = displayTime;
    endInfo.environmentBlendMode = envBlendMode;
    endInfo.layerCount = (hud.swapchain != XR_NULL_HANDLE) ? 2 : 1;
    endInfo.layers = layers;

    return XR_SUCCEEDED(xrEndFrame(session, &endInfo));
}

bool SubmitWindowSpaceLayersFrame(
    XrSession session,
    XrSpace localSpace,
    XrTime displayTime,
    XrEnvironmentBlendMode envBlendMode,
    const XrCompositionLayerProjectionView* projViews,
    uint32_t viewCount,
    const WindowSpaceLayerDesc* layers,
    uint32_t count,
    const void* projectionNext)
{
    XrCompositionLayerProjection projLayer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    projLayer.next = projectionNext;
    projLayer.space = localSpace;
    projLayer.viewCount = viewCount;
    projLayer.views = projViews;

    // Storage must outlive the xrEndFrame call — the runtime reads these
    // structs by pointer during submission.
    std::vector<XrCompositionLayerWindowSpaceDXR> wsLayers;
    wsLayers.reserve(count);
    std::vector<const XrCompositionLayerBaseHeader*> headers;
    headers.reserve(count + 1);
    headers.push_back((const XrCompositionLayerBaseHeader*)&projLayer);

    for (uint32_t i = 0; i < count; i++) {
        const WindowSpaceLayerDesc& d = layers[i];
        if (d.sc == nullptr || d.sc->swapchain == XR_NULL_HANDLE) {
            continue;
        }
        int32_t srcW = (d.srcW < 0) ? (int32_t)d.sc->width : d.srcW;
        int32_t srcH = (d.srcH < 0) ? (int32_t)d.sc->height : d.srcH;

        XrCompositionLayerWindowSpaceDXR layer = {};
        layer.type = (XrStructureType)XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_DXR;
        layer.next = nullptr;
        layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        layer.subImage.swapchain = d.sc->swapchain;
        layer.subImage.imageRect.offset = { d.srcX, d.srcY };
        layer.subImage.imageRect.extent = { srcW, srcH };
        layer.subImage.imageArrayIndex = 0;
        layer.x = d.x;
        layer.y = d.y;
        layer.width = d.width;
        layer.height = d.height;
        layer.disparity = d.disparity;

        wsLayers.push_back(layer);
    }
    // Append header pointers after the vector is fully populated so a
    // reallocation during push_back can't leave dangling pointers.
    for (const auto& l : wsLayers) {
        headers.push_back((const XrCompositionLayerBaseHeader*)&l);
    }

    XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
    endInfo.displayTime = displayTime;
    endInfo.environmentBlendMode = envBlendMode;
    endInfo.layerCount = (uint32_t)headers.size();
    endInfo.layers = headers.data();

    return XR_SUCCEEDED(xrEndFrame(session, &endInfo));
}

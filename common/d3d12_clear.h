// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Display-referred clears for Direct3D 12 (runtime #1647).
 *
 * Header-only and Windows-only (displayxr-common links no D3D12; only D3D12
 * apps include this). The rule and the reasoning live in `clear_policy.h`.
 *
 * ── The D3D12 asymmetry, which is the whole reason this file exists ─────────
 *
 * The D3D11 twin can interrogate the target it was handed:
 * `ID3D11RenderTargetView::GetDesc()` yields the view's format, and the view is
 * what arms the hardware encode. **D3D12 has no such call.** A
 * `D3D12_CPU_DESCRIPTOR_HANDLE` is an opaque address in a descriptor heap; it
 * carries no format and cannot be queried back. So a D3D12 caller MUST pass
 * the `DXGI_FORMAT` it created the RTV with.
 *
 * That is not a wart to route around. The runtime hands out **TYPELESS**
 * swapchain textures, so the resource format answers nothing on either API —
 * the view decides. `DxgiClearValueSpace()` therefore returns `Unknown` for the
 * `_TYPELESS` codes on purpose, and passing the texture's format instead of the
 * RTV's earns a warning rather than a wrong colour.
 *
 * @code
 *   // The same DXGI_FORMAT the RTV was created with — NOT the resource's.
 *   const auto fmt = (DXGI_FORMAT)xr.swapchain.format;
 *   dxr::ClearRenderTargetViewDisplayReferred(cmdList, rtvHandle, fmt, kBackground);
 * @endcode
 *
 * For a pipeline where a later blit/resolve does the encoding, pass
 * `dxr::ClearValueSpace` instead of a format — see `vk_clear.h`'s file comment,
 * which spells that case out.
 *
 * Alpha is never converted. An unclassified format is cleared with the
 * authored value and warned about once.
 */

#pragma once

#include <d3d12.h>

#include "clear_policy.h"

namespace dxr {

//! Clear an RTV whose space the caller states outright.
inline void
ClearRenderTargetViewDisplayReferred(ID3D12GraphicsCommandList *cmdList,
                                     D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                                     ClearValueSpace space,
                                     const float displayReferredRGBA[4],
                                     UINT numRects = 0,
                                     const D3D12_RECT *rects = nullptr)
{
    float c[4];
    ApplyClearValueSpace(space, displayReferredRGBA, c);
    cmdList->ClearRenderTargetView(rtv, c, numRects, rects);
}

//! Clear an RTV, deriving the space from the format the RTV was CREATED with
//! (not the resource's — see the file comment).
inline void
ClearRenderTargetViewDisplayReferred(ID3D12GraphicsCommandList *cmdList,
                                     D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                                     DXGI_FORMAT rtvFormat,
                                     const float displayReferredRGBA[4],
                                     UINT numRects = 0,
                                     const D3D12_RECT *rects = nullptr)
{
    const ClearValueSpace space = DxgiClearValueSpace((int64_t)rtvFormat);
    if (space == ClearValueSpace::Unknown) {
        ReportUnknownClearTarget("D3D12", (long long)rtvFormat);
    }
    ClearRenderTargetViewDisplayReferred(cmdList, rtv, space, displayReferredRGBA, numRects, rects);
}

} // namespace dxr

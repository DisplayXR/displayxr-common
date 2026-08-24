// Copyright 2026, DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Load-time asset auto-fit: bounding box + viewport -> rig vHeight.
 *
 * One shared rule for framing a just-loaded asset (GLB/FBX scene, gaussian
 * splat, avatar) on the display rig: pick the virtualDisplayHeight so the
 * rendered asset fills no more than `fill` (default 80%) of the viewport in
 * BOTH axes. The display rig maps a world-space slab of height vHeight onto
 * the canvas (m2v = vHeight / canvas_height_m), so at the display plane an
 * asset of world size W x H occupies H / vHeight of the viewport vertically
 * and W / (vHeight * aspect) horizontally — the two 80% caps collapse to
 *
 *     vHeight = max(H, W / aspect) / fill,   aspect = viewportW / viewportH
 *
 * Platform-neutral and pure; the caller owns the bounds source
 * (getRobustSceneBounds / getMainObjectBounds), the viewport source (window
 * client rect, or the 3D-zone rect when rendering into a display zone), the
 * fallback vHeight, and any recompute policy (this is load-time framing; the
 * fill fraction is exact for content at the display plane and nominal for
 * content in front of / behind it).
 */
#pragma once

#include <cstdint>

namespace dxr {

//! Default fill fraction: the binding axis of the asset spans 80% of the
//! viewport, leaving 10% margins on each side.
inline constexpr float kAutoFitDefaultFill = 0.8f;

//! Compute the display-rig virtualDisplayHeight framing an asset of
//! world-space size extentW x extentH (FULL sizes, not half-extents) inside a
//! viewport of viewportW x viewportH so neither rendered axis exceeds `fill`
//! of the viewport. Viewport dims can be pixels or meters — only the aspect
//! ratio matters (assumes square pixels when passing pixels; pass the 3D-zone
//! rect, not the window, when the asset renders into a zone).
//!
//! Degrades gracefully: a non-positive extentW or viewport falls back to the
//! height-only fit (the pre-width-rule behavior). Returns 0.0f when no fit is
//! computable (non-positive extentH or fill) — callers keep their existing
//! `if (!(vh > 1e-3f)) vh = fallback;` guard.
inline float
AutoFitVHeight(float extentW,
               float extentH,
               float viewportW,
               float viewportH,
               float fill = kAutoFitDefaultFill)
{
	if (!(extentH > 0.0f) || !(fill > 0.0f)) {
		return 0.0f;
	}
	float vh = extentH / fill;
	if (extentW > 0.0f && viewportW > 0.0f && viewportH > 0.0f) {
		const float aspect = viewportW / viewportH;
		const float vhForWidth = extentW / (fill * aspect);
		if (vhForWidth > vh) {
			vh = vhForWidth;
		}
	}
	return vh;
}

//! Recover the PANEL (window) pixel size from a rendering mode's per-view
//! recommended rect and that same mode's view scale:
//!
//!     panel = view_px / view_scale
//!
//! This is the viewport `AutoFitVHeight` wants on a fullscreen app that has no
//! window rect of its own to ask for (the Android legs, where the runtime owns
//! the surface). An app that CAN read its live window/canvas rect should pass
//! that instead — and a zone-rendering app must pass the zone rect — this is
//! the load-time bootstrap for the case where neither is available yet.
//!
//! DO NOT reconstruct the panel as `view_px * tile_grid`. That yields the
//! ATLAS, not the panel, and the two coincide only in the special case
//! `view_scale == 1 / tile_count`. The Leia Android LeiaSR mode breaks it:
//! 2x1 tiles with scale 0.750x0.750 on a 2560x1600 panel gives per-view
//! 1920x1200, so the tile reconstruction reports 3840x1200 (aspect 3.200)
//! where the panel is 2560x1600 (aspect 1.600). Feeding that 2x-too-wide
//! aspect to AutoFitVHeight halves its `extentW / (fill * aspect)` term, so
//! the width cap silently stops binding and the fit degrades to height-only.
//! Dividing by the scale is correct for ANY tiling, isotropic or not.
//!
//! Returns false (and leaves the outputs untouched) when the inputs cannot
//! produce a panel size, so callers keep whatever fallback viewport they had.
inline bool
PanelPixelsFromView(uint32_t viewWidthPixels,
                    uint32_t viewHeightPixels,
                    float viewScaleX,
                    float viewScaleY,
                    float &outPanelW,
                    float &outPanelH)
{
	if (viewWidthPixels == 0u || viewHeightPixels == 0u) {
		return false;
	}
	if (!(viewScaleX > 0.0f) || !(viewScaleY > 0.0f)) {
		return false;
	}
	outPanelW = (float)viewWidthPixels / viewScaleX;
	outPanelH = (float)viewHeightPixels / viewScaleY;
	return true;
}

} // namespace dxr

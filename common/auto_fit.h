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

} // namespace dxr

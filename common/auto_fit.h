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
 * client rect, or the 3D-zone rect when rendering into a display zone), and the
 * fallback vHeight. The fill fraction is exact for content at the display plane
 * and nominal for content in front of / behind it.
 *
 * WHEN TO RECOMPUTE -- read this before calling it once at load.
 *
 * vHeight is a function of (content, viewport). It is NOT load-time-only
 * framing: any change to the viewport invalidates it. Rotating a tablet is the
 * loud case -- a 2560x1600 panel held portrait is a 1600x2560 viewport, and a
 * fit derived for one renders ~1.9x wrong in the other (measured). A window
 * resize or a zone-rect change is the same thing, quieter.
 *
 * So: re-derive on every viewport change, and PRESERVE THE USER'S DEVIATION.
 * Zoom, orbit and pivot are deliberate user state; a viewport change is not a
 * request to undo them. Apps that keep the user's zoom as a separate
 * multiplicative term (rig_vh = base / zoom, or a content-side scale) get this
 * for free: move the base, touch nothing else, and a 2x pinch stays 2x of the
 * NEW fit so the subject keeps its apparent size. Recentring belongs on an
 * explicit reset gesture, not on rotation.
 *
 * Two practical notes, both learned the hard way:
 *   - Gate on the ASPECT, not the pixel size: an intermediate size mid-rotation
 *     that lands on the same aspect must not retrigger, and an in-flight
 *     transition should RETARGET rather than restart.
 *   - Cache the fit extents. They are properties of the CONTENT, so a viewport
 *     change re-derives the base without re-measuring the scene.
 *
 * Use FitTransition below to animate the move -- a ~2x jump is very visible.
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
//! `view_scale == 1 / tile_count`. A shipping 3D mode breaks it: 2x1 tiles
//! with scale 0.750x0.750 on a 2560x1600 panel gives per-view 1920x1200, so
//! the tile reconstruction reports 3840x1200 (aspect 3.200) where the panel
//! is 2560x1600 (aspect 1.600). Feeding that 2x-too-wide
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

//! Animated scalar move between two vHeight values, for the viewport-change
//! refit described above. A rotation can nearly double the base, and snapping
//! that reads as a glitch.
//!
//! Header-only on purpose. RigTransition (common/rig_transition.h) already does
//! this properly for whole dxr_rig snapshots, but it lives in a .cpp inside
//! displayxr_common_lib, which an Android build cannot link -- so every Android
//! leg that wanted it inlined its own copy. This is the same SmoothStep curve
//! and the same "starts landed" convention, reachable from displayxr::rules.
//!
//! Usage:
//!     if (aspect_changed) fit.start(current_vh, AutoFitVHeight(...));
//!     if (fit.update(dt, &vh)) apply(vh);
struct FitTransition
{
	//! Begin (or RETARGET) a move to `to`. Retargeting mid-flight keeps the
	//! current value as the new origin, so a rotation that settles in two
	//! steps cannot snap back to where it started.
	void start(float from, float to, float duration_s = 0.2f)
	{
		from_ = from;
		to_ = to;
		dur_ = (duration_s > 0.0f) ? duration_s : 0.0f;
		t_ = (dur_ > 0.0f) ? 0.0f : 1.0f;
	}

	//! Advance by `dt_s` and write the interpolated value. Returns false once
	//! landed, so callers can skip redundant work.
	//!
	//! Clamp `dt_s` yourself if the app can be backgrounded: a multi-second
	//! frame would otherwise land the move instantly, which is the snap this
	//! exists to avoid.
	bool update(float dt_s, float *out)
	{
		if (t_ >= 1.0f) {
			return false;
		}
		t_ += (dur_ > 0.0f) ? (dt_s / dur_) : 1.0f;
		if (t_ > 1.0f) {
			t_ = 1.0f;
		}
		if (out != nullptr) {
			*out = value();
		}
		return true;
	}

	//! Current interpolated value without advancing.
	float value() const
	{
		const float c = curve(t_);
		return from_ + (to_ - from_) * c;
	}

	bool active() const { return t_ < 1.0f; }
	float target() const { return to_; }

	//! Hermite 3t^2-2t^3 -- identical to RigTransition's Easing::SmoothStep.
	static float curve(float t)
	{
		if (t <= 0.0f) {
			return 0.0f;
		}
		if (t >= 1.0f) {
			return 1.0f;
		}
		return t * t * (3.0f - 2.0f * t);
	}

private:
	float from_ = 0.0f;
	float to_ = 0.0f;
	float dur_ = 0.0f;
	float t_ = 1.0f; //!< normalized progress; starts at 1 (idle/landed)
};

} // namespace dxr

// Copyright 2026, DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Where the auto-fit viewport comes from, and when it changed.
 *
 * auto_fit.h says vHeight is a function of (content, viewport) and to
 * re-derive on every viewport change. This file answers the question that
 * leaves open on the DESKTOP legs: WHICH rectangle is the viewport?
 *
 * The obvious answer -- the app's own window client rect -- is wrong under a
 * spatial workspace, and wrong in a way that reads as a content bug rather
 * than a viewport one. A shell-launched app is composed into a 3D window tile
 * the shell owns; its own HWND/NSWindow is hidden, is never what the user
 * sees, and carries whatever size the app happened to create it at. Worse,
 * the runtime only resizes that hidden window to the tile LATER (deferred and
 * async, once the client is placed), so an asset auto-loaded during init
 * frames against the creation size every time. Measured on the gaussian-splat
 * demo: every run, standalone and shell, fitted against 1280x720 / aspect
 * 1.778 -- the window creation size -- while the tile was square. A 2.01x1.74
 * scene then came out ~15% oversized and overflowed the sides.
 *
 * The runtime already publishes the right rectangle. XR_DXR_view_rig's raw
 * channel (XrViewDisplayRawDXR) reports the canvas the runtime RESOLVED for
 * the locate: the shell tile under a workspace, the window client rect
 * standalone, and the zone rect under a zone-scoped locate. That is the
 * viewport in all three cases, from one source, with no per-mode branching in
 * the app.
 *
 * Its one limitation drives the whole design here: it does not exist until the
 * first successful locate, which is after init -- exactly when the load-time
 * fit wants it. So this is NOT a "read the canvas" helper. It is a canvas that
 * ARRIVES: fit against whatever you have, publish the real canvas when the
 * first frame produces it, and let the aspect gate refit. `Changed()` returns
 * true on that first publish for free, because the fitted aspect is still the
 * bootstrap one.
 *
 * Usage (desktop; pair with dxr::FitTransition from auto_fit.h to animate):
 *
 *     // render thread, right where the raw channel is consumed
 *     g_canvas.PublishFromRaw(raw.canvasSizeMeters.width,
 *                             raw.canvasSizeMeters.height,
 *                             raw.canvasRectPx.extent.width,
 *                             raw.canvasRectPx.extent.height);
 *
 *     // fit / refit
 *     float w, h;
 *     g_canvas.Viewport(fallbackW, fallbackH, w, h);   // fallback = client rect
 *     if (dxr::AutoFitAspectChanged(g_fitAspect, w / h)) {
 *         fit.start(currentBase, dxr::AutoFitVHeight(extW, extH, w, h));
 *         g_fitAspect = w / h;
 *     }
 *
 * Android does not need this: there the app window IS the viewport and
 * ANativeWindow_getWidth/Height is already live and correct. Keep this
 * header-only and dependency-free (no OpenXR types -- callers pass the raw
 * channel's fields as plain numbers) so displayxr::rules stays consumable
 * everywhere.
 */
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

namespace dxr {

//! Aspect-change tolerance for the refit gate. Gate on the ASPECT rather than
//! the pixel size (auto_fit.h): a resize that keeps proportions must not
//! retrigger, and neither must an intermediate size mid-transition.
inline constexpr float kAutoFitAspectTolerance = 1e-3f;

//! True when `live` differs enough from `fitted` to be worth re-deriving.
//! A non-positive `fitted` means "never fitted against a real viewport", which
//! always counts as changed -- that is what makes the first canvas publish
//! land the bootstrap fit without any extra flag.
inline bool
AutoFitAspectChanged(float fittedAspect, float liveAspect, float tol = kAutoFitAspectTolerance)
{
	if (!(liveAspect > 0.0f)) {
		return false;
	}
	if (!(fittedAspect > 0.0f)) {
		return true;
	}
	return std::fabs(liveAspect - fittedAspect) >= tol;
}

/*!
 * @brief The runtime-resolved canvas, published by the render thread and read
 * by whichever thread runs the fit.
 *
 * Deliberately just two atomics: the fit and the publish sit on different
 * threads in every current consumer (a scene load can run on the main thread
 * during init, or on the render thread when queued), and a torn width/height
 * pair only ever costs one frame of a slightly wrong aspect that the next
 * publish corrects. Nothing here needs a lock.
 */
class AutoFitCanvas
{
public:
	//! Publish from XR_DXR_view_rig's raw channel. Meters win over pixels:
	//! both describe the same canvas, but the meters are what the rig's
	//! m2v actually divides by, so they stay right even if the panel's
	//! pixels are not square. Pixels are the fallback for a runtime that
	//! filled only the rect. Ignores an empty publish (a locate that
	//! resolved no canvas) rather than clobbering a good one with zeros.
	//! Returns true when something was accepted.
	bool PublishFromRaw(float metersW, float metersH, int32_t pixelsW, int32_t pixelsH)
	{
		if (metersW > 0.0f && metersH > 0.0f) {
			Publish(metersW, metersH);
			return true;
		}
		if (pixelsW > 0 && pixelsH > 0) {
			Publish((float)pixelsW, (float)pixelsH);
			return true;
		}
		return false;
	}

	//! Publish a canvas directly (already-validated positive dims).
	void Publish(float w, float h)
	{
		w_.store(w, std::memory_order_relaxed);
		h_.store(h, std::memory_order_relaxed);
	}

	//! Has a real canvas arrived yet?
	bool Valid() const
	{
		return w_.load(std::memory_order_relaxed) > 0.0f && h_.load(std::memory_order_relaxed) > 0.0f;
	}

	//! Aspect of the published canvas, or 0 when none has arrived.
	float Aspect() const
	{
		const float w = w_.load(std::memory_order_relaxed);
		const float h = h_.load(std::memory_order_relaxed);
		return (w > 0.0f && h > 0.0f) ? (w / h) : 0.0f;
	}

	//! The viewport to fit against: the runtime canvas when it has arrived,
	//! else the caller's fallback (the app's own client rect -- correct
	//! standalone, and the best guess before the first locate). Returns true
	//! when the runtime canvas was used.
	//!
	//! Only the ASPECT of the result is meaningful to AutoFitVHeight, which
	//! is what lets meters and pixels be mixed across the two sources.
	bool Viewport(float fallbackW, float fallbackH, float &outW, float &outH) const
	{
		const float w = w_.load(std::memory_order_relaxed);
		const float h = h_.load(std::memory_order_relaxed);
		if (w > 0.0f && h > 0.0f) {
			outW = w;
			outH = h;
			return true;
		}
		outW = fallbackW;
		outH = fallbackH;
		return false;
	}

private:
	std::atomic<float> w_{0.0f};
	std::atomic<float> h_{0.0f};
};

} // namespace dxr

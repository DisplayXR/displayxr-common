// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The session's display rendering modes, looked up by meaning (header-only).
 *
 * THE TWO-MODE CONTRACT (runtime issue #2766): a display driver exposes
 * exactly two rendering modes at run time — one flat 2D mode and one 3D mode
 * (the 3D "kind" is a driver setting applied at start-up, not something an app
 * picks). An app therefore needs two questions answered, not a list walk:
 * "which mode is 2D?" and "which mode is 3D?". dxr::ModeTable answers them
 * from xrEnumerateDisplayRenderingModesDXR.
 *
 * It is written so it ALSO works against an older runtime that still reports
 * three to eight (or more) modes: nothing here assumes the count is two, the
 * table has no fixed cap, and each mode's `modeIndex` is kept exactly as the
 * runtime reported it — never assumed equal to its position in the array.
 * Use `isTwoMode()` to decide whether to show two-mode UI ("V=2D/3D") or the
 * older per-mode UI ("1-N").
 *
 * Usage (any platform):
 * @code
 *   dxr::ModeTable modes;
 *   modes.refresh(pfnEnumerateDisplayRenderingModesDXR, session); // after xrCreateSession
 *   // ... on XrEventDataRenderingModeChangedDXR:
 *   if (modes.refresh(pfnEnumerateDisplayRenderingModesDXR, session)) {
 *       // a mode's view count / tile layout / scale changed: re-derive sizes
 *   }
 *   uint32_t want2D = modes.flatModeIndex();
 *   int32_t  want3D = modes.stereoModeIndex();   // -1 = no 3D mode
 * @endcode
 *
 * Header-only, no compiled source, no platform dependency. Needs
 * <openxr/openxr.h> and <openxr/XR_DXR_display_info.h> on the include path
 * (every leg that calls the enumerate already has both), so it is reachable
 * through displayxr::rules as well as displayxr::common.
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

#include <openxr/openxr.h>
#include <openxr/XR_DXR_display_info.h>

namespace dxr {

//! Rendering-mode table. One instance per session.
class ModeTable
{
public:
	using ModeInfo = XrDisplayRenderingModeInfoDXR;

	//! The two-call enumerate, abstracted so a test (or a non-PFN caller) can
	//! inject it. Same contract as xrEnumerateDisplayRenderingModesDXR minus
	//! the session: (capacityInput, countOutput, modes).
	using EnumerateFn = std::function<XrResult(uint32_t, uint32_t *, ModeInfo *)>;

	//! Re-read the table through the extension function. A null pfn or session
	//! leaves the table unchanged and returns false.
	//! @return true iff the mode count changed, or any mode's viewCount,
	//!         tileColumns, tileRows, viewScaleX or viewScaleY changed (matched
	//!         by modeIndex). Only the active / requestable flags changing is
	//!         NOT a change in this sense (that is an ordinary mode switch).
	bool
	refresh(PFN_xrEnumerateDisplayRenderingModesDXR pfn, XrSession session)
	{
		if (pfn == nullptr || session == XR_NULL_HANDLE) {
			return false;
		}
		return refresh([pfn, session](uint32_t cap, uint32_t *count, ModeInfo *modes) {
			return pfn(session, cap, count, modes);
		});
	}

	//! Re-read the table through an injected enumerate (see EnumerateFn). On
	//! any enumerate failure the previous table is kept and false is returned;
	//! lastRefreshOk() then reports false.
	bool
	refresh(const EnumerateFn &enumerate)
	{
		std::vector<ModeInfo> fresh;
		ok_ = false;
		if (!enumerate) {
			return false;
		}
		// Two-call idiom. The count may grow between the calls (a hot-plug),
		// so retry a few times on XR_ERROR_SIZE_INSUFFICIENT.
		bool got = false;
		for (int attempt = 0; attempt < 4 && !got; attempt++) {
			uint32_t count = 0;
			XrResult r = enumerate(0, &count, nullptr);
			if (XR_FAILED(r)) {
				return false;
			}
			fresh.assign(count, blankInfo());
			if (count == 0) {
				got = true; // a runtime with no modes (yet): an empty table
				break;
			}
			r = enumerate(count, &count, fresh.data());
			if (r == XR_ERROR_SIZE_INSUFFICIENT) {
				continue;
			}
			if (XR_FAILED(r)) {
				return false;
			}
			if (count < fresh.size()) {
				fresh.resize(count);
			}
			got = true;
		}
		if (!got) {
			return false;
		}
		ok_ = true;
		return assign(fresh.data(), (uint32_t)fresh.size());
	}

	//! Replace the table with caller-provided entries (already enumerated, or
	//! test data). Each entry's `next` is cleared in the copy. Same return
	//! value as refresh().
	bool
	assign(const ModeInfo *modes, uint32_t count)
	{
		std::vector<ModeInfo> fresh;
		fresh.reserve(count);
		for (uint32_t i = 0; modes != nullptr && i < count; i++) {
			ModeInfo m = modes[i];
			m.next = nullptr;
			m.modeName[XR_MAX_SYSTEM_NAME_SIZE - 1] = '\0';
			fresh.push_back(m);
		}
		const bool changed = layoutDiffers(modes_, fresh);
		modes_.swap(fresh);
		return changed;
	}

	//! Empty the table (e.g. on session destroy).
	void
	clear()
	{
		modes_.clear();
		ok_ = false;
	}

	//! True when the most recent refresh() call enumerated successfully.
	bool
	lastRefreshOk() const
	{
		return ok_;
	}

	size_t
	size() const
	{
		return modes_.size();
	}
	bool
	empty() const
	{
		return modes_.empty();
	}
	//! The two-mode contract (runtime #2766): exactly one 2D + one 3D mode.
	bool
	isTwoMode() const
	{
		return modes_.size() == 2;
	}
	//! The entries in the order the runtime reported them.
	const std::vector<ModeInfo> &
	modes() const
	{
		return modes_;
	}

	//! The entry whose modeIndex is `modeIndex`, or nullptr.
	const ModeInfo *
	find(uint32_t modeIndex) const
	{
		for (const ModeInfo &m : modes_) {
			if (m.modeIndex == modeIndex) {
				return &m;
			}
		}
		return nullptr;
	}

	//! The entry the runtime flagged isActive (v13+), or nullptr (older runtime,
	//! or an empty table).
	const ModeInfo *
	active() const
	{
		for (const ModeInfo &m : modes_) {
			if (m.isActive) {
				return &m;
			}
		}
		return nullptr;
	}

	//! A flat mode: one view, display hardware not in 3D.
	static bool
	isFlat(const ModeInfo &m)
	{
		return m.viewCount == 1 && !m.hardwareDisplay3D;
	}
	//! A 3D mode for lookup purposes: two or more views.
	static bool
	isStereo(const ModeInfo &m)
	{
		return m.viewCount >= 2;
	}

	//! The 2D mode: the first requestable 1-view mode with hardwareDisplay3D
	//! false; else the entry with modeIndex 0; else nullptr.
	const ModeInfo *
	flat() const
	{
		for (const ModeInfo &m : modes_) {
			if (m.isRequestable && isFlat(m)) {
				return &m;
			}
		}
		return find(0);
	}

	//! The 3D mode: the active mode if it is 3D (so a 3D->3D no-op stays put on
	//! an older multi-mode runtime); else the first requestable mode with
	//! hardwareDisplay3D true and viewCount >= 2; else the first mode with
	//! viewCount >= 2; else nullptr.
	const ModeInfo *
	stereo() const
	{
		const ModeInfo *a = active();
		if (a != nullptr && isStereo(*a)) {
			return a;
		}
		for (const ModeInfo &m : modes_) {
			if (m.isRequestable && m.hardwareDisplay3D && isStereo(m)) {
				return &m;
			}
		}
		for (const ModeInfo &m : modes_) {
			if (isStereo(m)) {
				return &m;
			}
		}
		return nullptr;
	}

	//! flat()'s modeIndex, or 0 when the table is empty (mode 0 is the 2D mode
	//! on every runtime that predates this table).
	uint32_t
	flatModeIndex() const
	{
		const ModeInfo *m = flat();
		return m != nullptr ? m->modeIndex : 0u;
	}
	//! stereo()'s modeIndex, or -1 when there is no 3D mode.
	int32_t
	stereoModeIndex() const
	{
		const ModeInfo *m = stereo();
		return m != nullptr ? (int32_t)m->modeIndex : -1;
	}

	//! The mode a V-style key should target from `currentMode`:
	//!  - two modes: toggle — flat() when currentMode is a 3D mode, stereo()
	//!    otherwise;
	//!  - more modes (older runtime): the next entry after currentMode in the
	//!    runtime's order, wrapping (the historical cycle);
	//!  - empty table: -1.
	int32_t
	cycleTarget(uint32_t currentMode) const
	{
		if (modes_.empty()) {
			return -1;
		}
		if (isTwoMode()) {
			const ModeInfo *cur = find(currentMode);
			if (cur != nullptr && isStereo(*cur)) {
				return (int32_t)flatModeIndex();
			}
			return stereoModeIndex();
		}
		for (size_t i = 0; i < modes_.size(); i++) {
			if (modes_[i].modeIndex == currentMode) {
				return (int32_t)modes_[(i + 1) % modes_.size()].modeIndex;
			}
		}
		return (int32_t)modes_[0].modeIndex;
	}

	//! The mode a digit key `digit` (0-8) should target, or -1 (inert):
	//!  - 0: flat();
	//!  - 1 with two modes: stereo();
	//!  - otherwise: the mode whose modeIndex is `digit`, if it exists (so the
	//!    per-mode digits keep working on an older multi-mode runtime and are
	//!    inert with two modes).
	int32_t
	digitTarget(int32_t digit) const
	{
		if (digit < 0 || modes_.empty()) {
			return -1;
		}
		if (digit == 0) {
			return (int32_t)flatModeIndex();
		}
		if (digit == 1 && isTwoMode()) {
			return stereoModeIndex();
		}
		return find((uint32_t)digit) != nullptr ? digit : -1;
	}

private:
	static ModeInfo
	blankInfo()
	{
		ModeInfo m;
		std::memset(&m, 0, sizeof(m));
		m.type = XR_TYPE_DISPLAY_RENDERING_MODE_INFO_DXR;
		m.next = nullptr;
		return m;
	}

	static bool
	layoutDiffers(const std::vector<ModeInfo> &a, const std::vector<ModeInfo> &b)
	{
		if (a.size() != b.size()) {
			return true;
		}
		for (const ModeInfo &n : b) {
			const ModeInfo *o = nullptr;
			for (const ModeInfo &m : a) {
				if (m.modeIndex == n.modeIndex) {
					o = &m;
					break;
				}
			}
			if (o == nullptr || o->viewCount != n.viewCount || o->tileColumns != n.tileColumns ||
			    o->tileRows != n.tileRows || o->viewScaleX != n.viewScaleX || o->viewScaleY != n.viewScaleY) {
				return true;
			}
		}
		return false;
	}

	std::vector<ModeInfo> modes_;
	bool ok_ = false;
};

} // namespace dxr

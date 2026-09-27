// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The window-drag decisions of displayxr::linux_window, as pure logic:
 *         when a drag starts and ends (X11 and Wayland), and which
 *         phase-correct position an X11 drag step lands on. No X11, Wayland or
 *         bus state, so every rule here is unit tested without a display
 *         (tests/linux_window_drag_test.cpp).
 *
 * ## Ending a drag (the right-button bug)
 *
 * A drag must end when the button that started it is released — wherever the
 * pointer is, however fast it moved. Two ways that failed:
 *
 * - **Wayland, mutter.** A content drag on a secondary button handed the drag
 *   to the compositor with `xdg_toplevel.move`. mutter accepts the request
 *   with ANY pressed button's serial (`meta_wayland_pointer_get_grab_info`
 *   only asks for button_count > 0), but its move grab ends only on the
 *   release of button 1 or of the resize button (mutter 50.1,
 *   `src/compositor/meta-window-drag.c`, `process_pointer_event`,
 *   CLUTTER_BUTTON_RELEASE: `button == 1 || button ==
 *   meta_prefs_get_mouse_button_resize ()` — 2, or 3 with
 *   resize-with-right-button). A right-button release is ignored, the grab
 *   keeps following the pointer until the next press of any button, and the
 *   client never sees the release (the grab owns the pointer). mutter itself
 *   never starts a move from a secondary button (Super+right is the window
 *   menu), which is why its grab does not handle one. wl_drag_mode() below
 *   keeps `xdg_toplevel.move` for button 1 and hands a secondary button to
 *   the window-geometry extension's pointer drag instead (placement
 *   capability 4), which ends on the button mask, not on a release event.
 * - **X11.** The helper owns the drag and ends it on the ButtonRelease of the
 *   drag button. A release that never reaches this client (a grab that did
 *   not take, a server-side grab by another client, a release routed
 *   elsewhere) left the window glued to the pointer. ButtonDrag also ends
 *   the drag when a MotionNotify's state, or the pointer state queried once
 *   per pump while dragging, shows the button up.
 *
 * ## Which position an X11 drag step lands on
 *
 * Under XWayland at a scaled output the window can only land on a quantum
 * lattice (every 2nd device px at 200 %), and the runtime's snap searches it
 * for a phase-correct position (runtime#1588). Two things went wrong:
 *
 * - **Misses.** The lattice is anchored at the drag origin. A window that was
 *   CREATED at an odd position has never been placed by the window manager,
 *   so its origin is not a reachable position and neither is anything on
 *   origin + 2Z^2: every move of that drag was rounded away by the server
 *   (57 of 58 in the log that reported this). X11Picker re-anchors the
 *   lattice at the first landing that differs from what was asked — a
 *   landed position is reachable by definition.
 * - **Swings.** The runtime answers one target with the FIRST phase-correct
 *   reachable point of a Chebyshev ring search, so consecutive steps of a
 *   straight drag land on either side of the drag line (±6–12 device px
 *   sideways). X11Picker asks the snap about every target around the raw
 *   position, keeps the distinct answers (all phase-correct and reachable),
 *   and lets Choice pick among them — the same rule the GNOME extension
 *   applies to a Wayland compositor drag (runtime#1748, extension v9).
 */
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dxr_drag {

/*
 *
 * Choice — which phase-correct position a drag move lands on.
 *
 */

/*!
 * C++ twin of `LatticeChoice` in the runtime's GNOME extension
 * (contrib/gnome-shell/window-geometry@displayxr.org/lib.js, extension v9):
 * the same constants and the same rule, so an X11 drag and a Wayland
 * compositor drag spend their error the same way.
 *
 * Every candidate is equally phase-correct; the only freedom is WHICH one a
 * move lands on. Plain nearest treats every direction alike, so along a
 * straight drag it picks candidates on either side of the drag line in turn,
 * and the window wiggles sideways. So the sideways error counts
 * kPerpWeight times the error along the drag (which only makes the window
 * lead or lag the pointer a little), the lead/lag is capped at kMaxAlongPx,
 * and past the cap — or before the drag has a direction — the choice is plain
 * nearest. Everything is in DEVICE px.
 */
struct Choice
{
	//! Sideways error weight relative to along-drag error (squared: 3x).
	static constexpr double kPerpWeight = 9.0;
	//! Largest lead or lag along the drag the choice may add, device px.
	static constexpr double kMaxAlongPx = 6.0;
	//! How fast the drag direction follows a turn, device px of travel.
	static constexpr double kDirMemoryPx = 8.0;
	//! Travel before the direction is trusted, device px (a drag from rest).
	static constexpr double kMinTravelPx = 4.0;

	double ax = 0.0, ay = 0.0; //!< decayed drag axis (not normalised)
	double lx = 0.0, ly = 0.0; //!< last raw displacement observed

	void
	reset()
	{
		*this = Choice{};
	}

	//! Feed the raw (pointer-derived) displacement of each move, device px.
	void
	observe(double dx, double dy)
	{
		const double mx = dx - lx, my = dy - ly;
		const double d = std::hypot(mx, my);
		if (d > 0.0) {
			// An AXIS: a drag that reverses keeps it, so a step against it
			// is folded over before it is added.
			const double sgn = (mx * ax + my * ay) < 0.0 ? -1.0 : 1.0;
			const double decay = std::exp(-d / kDirMemoryPx);
			ax = ax * decay + sgn * mx;
			ay = ay * decay + sgn * my;
		}
		lx = dx;
		ly = dy;
	}

	/*!
	 * @p for_each(cb) calls cb(ex, ey) for every candidate displacement.
	 * Returns false when there is none. @p isotropic forces plain nearest.
	 */
	template <typename ForEach>
	bool
	choose(const ForEach &for_each, double dx, double dy, int32_t *out_x, int32_t *out_y, bool isotropic = false) const
	{
		const double n = std::hypot(ax, ay);
		const bool aniso = !isotropic && n >= kMinTravelPx;
		const double ux = aniso ? ax / n : 1.0, uy = aniso ? ay / n : 0.0;
		bool have_best = false, have_near = false;
		double best_c = 0.0, near_c = 0.0;
		int32_t bx = 0, by = 0, nx = 0, ny = 0;
		for_each([&](int32_t ex, int32_t ey) {
			const double cx = ex - dx, cy = ey - dy;
			const double d = cx * cx + cy * cy;
			if (!have_near || d < near_c) {
				have_near = true;
				near_c = d;
				nx = ex;
				ny = ey;
			}
			if (!aniso) {
				return;
			}
			const double al = cx * ux + cy * uy, pe = cy * ux - cx * uy;
			if (std::fabs(al) > kMaxAlongPx) {
				return;
			}
			const double c = al * al + kPerpWeight * pe * pe;
			if (!have_best || c < best_c) {
				have_best = true;
				best_c = c;
				bx = ex;
				by = ey;
			}
		});
		if (have_best) {
			*out_x = bx;
			*out_y = by;
			return true;
		}
		if (have_near) {
			*out_x = nx;
			*out_y = ny;
			return true;
		}
		return false;
	}
};

/*
 *
 * X11Picker — the X11 drag step: candidates around the raw target, then Choice.
 *
 */

struct Pt
{
	int32_t x = 0, y = 0;
};

/*!
 * The snap, asked about several targets at once: fills @p out (same length and
 * order as @p targets) with each target's snapped position. All positions are
 * ABSOLUTE root px; the snap's origin (the lattice anchor) is bound by the
 * caller. Returns false when nothing snaps (no provider, the display processor
 * declined) — the drag then goes to the raw target.
 */
using SnapMany = std::function<bool(const std::vector<Pt> &targets, std::vector<Pt> *out)>;

/*!
 * One X11 drag's target selection.
 *
 * The snap is a pure query: "from the anchor, where near here may the window
 * land?". Every answer is a position the display processor calls
 * phase-correct and — under a placement quantum — one the window can reach
 * (the runtime searches the reachable lattice, runtime#1588). The answer to a
 * single target is only the first such position a ring search found, so this
 * asks about every target within kRadius px of the raw one (memoised: a
 * continuing drag only asks about the new edge of the box) and chooses among
 * the distinct answers with Choice.
 *
 * The anchor starts at the drag origin and moves to where the window actually
 * landed whenever a landing differs from what was asked (reanchor()): only a
 * landed position is known to be reachable. The phase reference moves with
 * it, which is right — phase continuity is about the position last SHOWN.
 */
class X11Picker
{
public:
	//! Targets asked about, either side of the raw one (device px).
	static constexpr int32_t kRadius = 7;

	struct Result
	{
		bool snapped = false; //!< false: no snap — go to the raw target
		int32_t x = 0, y = 0; //!< where to move (absolute)
		uint32_t candidates = 0;
		uint32_t asked = 0; //!< targets sent to the snap for this step
	};

	//! A new drag from @p origin (the window's root origin at the press).
	void
	begin(int32_t origin_x, int32_t origin_y)
	{
		m_origin_x = m_anchor_x = origin_x;
		m_origin_y = m_anchor_y = origin_y;
		m_choice.reset();
		m_memo.clear();
		m_declined = false;
		m_reanchors = 0;
	}

	//! The window landed at (@p x, @p y), not where it was asked to: that is a
	//! reachable position, so the lattice is anchored there from now on.
	void
	reanchor(int32_t x, int32_t y)
	{
		if (x == m_anchor_x && y == m_anchor_y) {
			return;
		}
		m_anchor_x = x;
		m_anchor_y = y;
		m_memo.clear(); // answers are relative to the anchor
		m_reanchors++;
	}

	int32_t
	anchor_x() const
	{
		return m_anchor_x;
	}
	int32_t
	anchor_y() const
	{
		return m_anchor_y;
	}
	uint32_t
	reanchors() const
	{
		return m_reanchors;
	}

	/*!
	 * Where a drag step towards the raw target (@p raw_x, @p raw_y) lands.
	 * @p snap asks the snap (anchored at anchor_x/y()) about many targets.
	 * @p isotropic: plain nearest (DXR_X11_DRAG_NEAREST=1, for A/B).
	 */
	Result
	pick(const SnapMany &snap, int32_t raw_x, int32_t raw_y, bool isotropic = false)
	{
		Result r;
		r.x = raw_x;
		r.y = raw_y;
		// The drag axis follows the POINTER, relative to the fixed drag origin.
		m_choice.observe((double)(raw_x - m_origin_x), (double)(raw_y - m_origin_y));
		if (m_declined || !snap) {
			return r;
		}
		std::vector<Pt> ask;
		for (int32_t j = -kRadius; j <= kRadius; j++) {
			for (int32_t i = -kRadius; i <= kRadius; i++) {
				const Pt t{raw_x + i, raw_y + j};
				if (m_memo.find(key(t)) == m_memo.end()) {
					ask.push_back(t);
				}
			}
		}
		if (!ask.empty()) {
			std::vector<Pt> out;
			if (!snap(ask, &out) || out.size() != ask.size()) {
				m_declined = true; // no snap at all: identity for the rest of this drag
				return r;
			}
			for (size_t k = 0; k < ask.size(); k++) {
				m_memo.emplace(key(ask[k]), out[k]);
			}
			r.asked = (uint32_t)ask.size();
		}
		// The distinct answers within the box: the candidates.
		std::vector<Pt> cands;
		for (int32_t j = -kRadius; j <= kRadius; j++) {
			for (int32_t i = -kRadius; i <= kRadius; i++) {
				const Pt a = m_memo[key(Pt{raw_x + i, raw_y + j})];
				bool dup = false;
				for (const Pt &c : cands) {
					if (c.x == a.x && c.y == a.y) {
						dup = true;
						break;
					}
				}
				if (!dup) {
					cands.push_back(a);
				}
			}
		}
		r.candidates = (uint32_t)cands.size();
		int32_t ex = 0, ey = 0;
		const double dx = (double)(raw_x - m_origin_x), dy = (double)(raw_y - m_origin_y);
		const bool ok = m_choice.choose(
		    [&](const std::function<void(int32_t, int32_t)> &cb) {
			    for (const Pt &c : cands) {
				    cb(c.x - m_origin_x, c.y - m_origin_y);
			    }
		    },
		    dx, dy, &ex, &ey, isotropic);
		if (ok) {
			r.snapped = true;
			r.x = m_origin_x + ex;
			r.y = m_origin_y + ey;
		}
		return r;
	}

private:
	static uint64_t
	key(const Pt &p)
	{
		return ((uint64_t)(uint32_t)p.x << 32) | (uint32_t)p.y;
	}

	int32_t m_origin_x = 0, m_origin_y = 0;
	int32_t m_anchor_x = 0, m_anchor_y = 0;
	Choice m_choice;
	std::unordered_map<uint64_t, Pt> m_memo;
	bool m_declined = false;
	uint32_t m_reanchors = 0;
};

/*!
 * Is a readback of the window after a move a LANDING the drag should believe?
 *
 * The move is asynchronous (a ConfigureRequest the window manager answers), so
 * a readback may still show the window where it was BEFORE the move: that is
 * "not placed yet", not a miss. A position that is neither the old one nor the
 * requested one is the window manager's answer — the requested position was
 * not reachable, and the one it chose is.
 */
enum class Landing
{
	Exact,   //!< landed where asked
	Pending, //!< still where it was: the move has not been answered yet
	Moved,   //!< the window manager put it somewhere else (reachable)
};

inline Landing
classify_landing(int32_t before_x, int32_t before_y, int32_t want_x, int32_t want_y, int32_t got_x, int32_t got_y)
{
	if (got_x == want_x && got_y == want_y) {
		return Landing::Exact;
	}
	if (got_x == before_x && got_y == before_y) {
		return Landing::Pending;
	}
	return Landing::Moved;
}

/*
 *
 * ButtonDrag — the X11 client-owned drag's start and end.
 *
 */

//! X11 core button masks (Button1Mask..Button5Mask), without Xlib.
inline unsigned
x11_button_mask(unsigned button)
{
	return (button >= 1 && button <= 5) ? (1u << (7 + button)) : 0u;
}

/*!
 * The X11 drag state machine. The window code feeds it what the server says
 * and acts on the answers; it holds no X state.
 *
 * Ends on, in order of normal to defensive:
 *  - the ButtonRelease of the drag button (release());
 *  - a MotionNotify whose state has the drag button UP (motion()): the
 *    release was not delivered to us, but the next motion says it happened;
 *  - the pointer state queried while dragging (poll()), which covers a
 *    release with no motion after it;
 *  - focus loss or an explicit cancel (fullscreen, a WM frame taking over).
 *
 * A drag that ended WITHOUT its release reports the button in
 * take_synthetic_release(): the app saw the press, so it is owed a release;
 * and the real release, if it still arrives later, is swallowed
 * (swallow_release()) so the app does not see two.
 */
class ButtonDrag
{
public:
	enum class End
	{
		NotEnded,
		Release,    //!< the drag button's ButtonRelease
		MotionUp,   //!< a MotionNotify with the button up (release lost)
		PollUp,     //!< the queried pointer state has the button up (release lost)
		FocusLost,  //!< the window lost focus mid-drag
		Cancelled,  //!< the window stopped being client-draggable
	};

	bool
	active() const
	{
		return m_active;
	}
	unsigned
	button() const
	{
		return m_button;
	}
	bool
	from_bar() const
	{
		return m_from_bar;
	}
	End
	last_end() const
	{
		return m_last_end;
	}

	//! A press of the drag button where a drag may start. False when one is
	//! already running (a second button during a drag never restarts it).
	bool
	press(unsigned button, bool from_bar = false)
	{
		note_press(button);
		if (m_active) {
			return false;
		}
		m_active = true;
		m_button = button;
		m_from_bar = from_bar;
		m_last_end = End::NotEnded;
		return true;
	}

	//! Any press of @p button (drag or not): a stale release of it we were
	//! waiting to swallow is not coming any more — the next one is the new
	//! press's.
	void
	note_press(unsigned button)
	{
		if (m_swallow == button) {
			m_swallow = 0;
		}
	}

	//! A ButtonRelease. True when it ended the drag.
	bool
	release(unsigned button)
	{
		if (!m_active || button != m_button) {
			return false;
		}
		finish(End::Release);
		return true;
	}

	//! A MotionNotify with its state. True when it ended the drag.
	bool
	motion(unsigned state)
	{
		return check_mask(state, End::MotionUp);
	}

	//! The pointer's button state, queried. True when it ended the drag.
	bool
	poll(unsigned state)
	{
		return check_mask(state, End::PollUp);
	}

	//! Focus lost (or any cancel). True when it ended a drag.
	bool
	cancel(End why = End::Cancelled)
	{
		if (!m_active) {
			return false;
		}
		finish(why);
		return true;
	}

	/*!
	 * The drag ended without its release reaching us: the button the app is
	 * owed a ButtonUp for (0 = none owed — a release ended it, or the bar
	 * started it and the app never saw the press). Once.
	 */
	unsigned
	take_synthetic_release()
	{
		const unsigned b = m_owed;
		m_owed = 0;
		return b;
	}

	//! Drop this ButtonRelease? True for the late real release of a drag that
	//! was already ended (and, for a bar drag, reported) without it. Once.
	bool
	swallow_release(unsigned button)
	{
		if (m_swallow != 0 && button == m_swallow) {
			m_swallow = 0;
			return true;
		}
		return false;
	}

private:
	bool
	check_mask(unsigned state, End why)
	{
		if (!m_active) {
			return false;
		}
		const unsigned mask = x11_button_mask(m_button);
		if (mask == 0 || (state & mask) != 0) {
			return false; // no mask for this button, or it is still held
		}
		finish(why);
		return true;
	}

	void
	finish(End why)
	{
		m_last_end = why;
		if (why != End::Release) {
			// The release may still be in flight: swallow it when it comes.
			m_swallow = m_button;
			// The app saw the press of a content drag; it is owed a release.
			m_owed = m_from_bar ? 0 : m_button;
		}
		m_active = false;
		m_button = 0;
		m_from_bar = false;
	}

	bool m_active = false;
	unsigned m_button = 0;
	bool m_from_bar = false;
	End m_last_end = End::NotEnded;
	unsigned m_owed = 0;
	unsigned m_swallow = 0;
};

/*
 *
 * Wayland — who runs a content drag, and what the app is told.
 *
 */

//! Who moves the window for a Wayland content drag.
enum class WlDragMode
{
	Nobody,         //!< nobody: this button cannot be ended on this compositor
	CompositorMove, //!< xdg_toplevel.move — the compositor's own move grab
	ShellPointer,   //!< the window-geometry extension follows the pointer (capability 4)
};

struct WlDragEnv
{
	//! The compositor is mutter (it advertises gtk_shell1, or the
	//! window-geometry extension is running): its move grab ends only on
	//! button 1 (or its resize button) — see the file comment.
	bool mutter = false;
	//! The window-geometry extension offers the pointer drag (capability 4).
	bool shell_pointer_drag = false;
};

/*!
 * The mode for a content drag on @p button (1 = left, 2 = middle, 3 = right,
 * 8/9 = side). Button 1 is always the compositor's move — every compositor
 * ends that grab on its release. A secondary button is too on a compositor
 * that ends the grab on any release; on mutter it is not, so it goes to the
 * extension's pointer drag, which ends on the button MASK (buttons 1-5 only:
 * Clutter has no mask for the side buttons), or to nobody.
 */
inline WlDragMode
wl_drag_mode(unsigned button, const WlDragEnv &env)
{
	if (button == 1) {
		return WlDragMode::CompositorMove;
	}
	if (!env.mutter) {
		return WlDragMode::CompositorMove;
	}
	if (env.shell_pointer_drag && button >= 2 && button <= 5) {
		return WlDragMode::ShellPointer;
	}
	return WlDragMode::Nobody;
}

/*!
 * One Wayland content drag, seen from the client.
 *
 * - CompositorMove: the grab takes the pointer (wl_pointer.leave) and the
 *   release goes to the grab, never to us. The drag is over when the pointer
 *   comes back (wl_pointer.enter) — the app, which saw the press, is owed the
 *   release then (on_enter()). A release that DOES reach us (the compositor
 *   refused the move) ends it normally.
 * - ShellPointer: nobody grabs, so the client keeps the implicit grab and
 *   sees the release (on_release()); the extension is told to stop then. A
 *   leave during it means the implicit grab is gone (the button went up
 *   somewhere we did not see, or a shell grab took the pointer): end too.
 *   The app sees no motion while the window follows the pointer, like the
 *   X11 drag.
 */
class WlContentDrag
{
public:
	WlDragMode
	mode() const
	{
		return m_mode;
	}
	unsigned
	button() const
	{
		return m_button;
	}
	bool
	active() const
	{
		return m_mode != WlDragMode::Nobody;
	}

	//! A press that started a drag in @p mode (Nobody: no drag).
	void
	begin(unsigned button, WlDragMode mode)
	{
		m_mode = mode;
		m_button = mode == WlDragMode::Nobody ? 0 : button;
	}

	//! What a pointer event means for the drag.
	struct Out
	{
		bool ended = false;           //!< the drag is over
		bool stop_shell = false;      //!< tell the extension to stop following
		unsigned synth_release = 0;   //!< emit a ButtonUp for this button first (0 = none)
	};

	//! A button release reached the client.
	Out
	on_release(unsigned button)
	{
		Out o;
		if (!active() || button != m_button) {
			return o;
		}
		o.ended = true;
		o.stop_shell = m_mode == WlDragMode::ShellPointer;
		end();
		return o;
	}

	//! The pointer entered the content surface.
	Out
	on_enter()
	{
		Out o;
		if (m_mode == WlDragMode::CompositorMove) {
			o.ended = true;
			o.synth_release = m_button;
			end();
		}
		return o;
	}

	//! The pointer left the content surface.
	Out
	on_leave()
	{
		Out o;
		if (m_mode == WlDragMode::ShellPointer) {
			o.ended = true;
			o.stop_shell = true;
			o.synth_release = m_button;
			end();
		}
		return o;
	}

	//! Motion is the window's own business while the extension moves it.
	bool
	suppress_motion() const
	{
		return m_mode == WlDragMode::ShellPointer;
	}

private:
	void
	end()
	{
		m_mode = WlDragMode::Nobody;
		m_button = 0;
	}

	WlDragMode m_mode = WlDragMode::Nobody;
	unsigned m_button = 0;
};

} // namespace dxr_drag

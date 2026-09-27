// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The window-drag decisions (common/linux/dxr_drag.h). No display.
 *
 * 1. Choice: the direction-aware pick among equally phase-correct positions
 *    (the C++ twin of the GNOME extension's LatticeChoice, runtime#1748).
 * 2. X11Picker over a model of the whole X11 path at 200 % under XWayland: a
 *    slanted-lens best-phase snap (the vendor weaver's shape: best phase
 *    within +-2 device px, period 2.76, slant 0.287), the runtime's
 *    reachable-lattice search around it (quantum 2, three Chebyshev rings —
 *    vk_snap_search_lattice), and a window manager that lands every request
 *    on the even lattice. Straight drags from an EVEN (settled) and an ODD
 *    (never placed) origin, slow and fast, at 0, 20, 45 and 90 degrees:
 *      - every step lands where it was asked, except at most the first of a
 *        drag from an unsettled origin (the old path missed ~every step);
 *      - sideways deviation from the drag line is never worse than the old
 *        path's, smaller on average, and bounded (<= 5.5 device px).
 * 3. classify_landing: a readback that still shows the old position is a
 *    pending move, not a miss.
 * 4. ButtonDrag (X11): press / release / motion-state / poll / focus / bar
 *    sequences, the synthetic release it owes the app and the late real
 *    release it swallows.
 * 5. Wayland: wl_drag_mode for every button on mutter and elsewhere, and
 *    WlContentDrag's press / leave / enter / release sequences.
 */
#include "dxr_drag.h"
#include "u_x11_scale.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace D = dxr_drag;

static int g_fail = 0;
#define CHECK(c)                                                                                                       \
	do {                                                                                                           \
		if (!(c)) {                                                                                            \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                              \
			g_fail++;                                                                                      \
		}                                                                                                      \
	} while (0)

//! Candidates for Choice::choose from a plain list.
struct List
{
	std::vector<std::pair<int32_t, int32_t>> e;
	template <typename Cb>
	void
	operator()(const Cb &cb) const
	{
		for (const auto &p : e) {
			cb(p.first, p.second);
		}
	}
};

static bool
same(int32_t x, int32_t y, int32_t ex, int32_t ey)
{
	return x == ex && y == ey;
}

/*
 * 1. Choice
 */
static void
test_choice()
{
	std::printf("1. Choice\n");
	int32_t x = 0, y = 0;
	{
		D::Choice c;
		c.observe(2, 0); // 2 device px: below kMinTravelPx
		CHECK(c.choose(List{{{6, 0}, {2, 2}}}, 2, 0, &x, &y) && same(x, y, 2, 2));
	}
	{
		D::Choice c;
		for (int i = 1; i <= 40; i++) {
			c.observe(i, 0);
		}
		// raw (40, 0): (44, 0) is 4 ahead, (40, 2) is 2 aside.
		CHECK(c.choose(List{{{44, 0}, {40, 2}}}, 40, 0, &x, &y) && same(x, y, 44, 0));
		CHECK(c.choose(List{{{44, 0}, {40, 2}}}, 40, 0, &x, &y, true) && same(x, y, 40, 2));
		// Past the lead/lag cap: not eligible.
		CHECK(c.choose(List{{{48, 0}, {40, 2}}}, 40, 0, &x, &y) && same(x, y, 40, 2));
		// Nothing within the cap: plain nearest, never no answer.
		CHECK(c.choose(List{{{50, 0}, {52, 0}}}, 40, 0, &x, &y) && same(x, y, 50, 0));
		CHECK(!c.choose(List{}, 40, 0, &x, &y));
		// An axis: reversing along the same line keeps it.
		for (int i = 39; i >= 20; i--) {
			c.observe(i, 0);
		}
		CHECK(c.choose(List{{{16, 0}, {20, 2}}}, 20, 0, &x, &y) && same(x, y, 16, 0));
		// A turn moves it.
		for (int j = 1; j <= 60; j++) {
			c.observe(20, j);
		}
		CHECK(c.choose(List{{{20, 64}, {22, 60}}}, 20, 60, &x, &y) && same(x, y, 20, 64));
	}
}

/*
 * 2. X11Picker over the modelled XWayland path.
 */
namespace model {

const double kPx = 2.76, kSlant = 0.287;

//! Phase error of a displacement from the drag's phase reference.
double
phase_err(int32_t dx, int32_t dy)
{
	return std::fabs(std::remainder((dx + kSlant * dy) / kPx, 1.0));
}

//! The vendor weaver's snap: best phase within +-2 device px of the target.
void
dp_snap(int32_t ox, int32_t oy, int32_t tx, int32_t ty, int32_t *sx, int32_t *sy)
{
	double best = 1e9;
	*sx = tx;
	*sy = ty;
	for (int32_t i = tx - 2; i <= tx + 2; i++) {
		for (int32_t j = ty - 2; j <= ty + 2; j++) {
			const double v = phase_err(i - ox, j - oy);
			if (v < best) {
				best = v;
				*sx = i;
				*sy = j;
			}
		}
	}
}

bool
reachable(int32_t anchor, int32_t v, int32_t q)
{
	return q <= 1 || ((v - anchor) % q) == 0;
}

//! The runtime's X11 snap under quantum @p q (vk_snap_search_lattice, anchored
//! at the origin it is given).
void
rt_snap(int32_t ox, int32_t oy, int32_t tx, int32_t ty, int32_t q, int32_t *out_x, int32_t *out_y)
{
	int32_t sx = 0, sy = 0;
	dp_snap(ox, oy, tx, ty, &sx, &sy);
	if (reachable(ox, sx, q) && reachable(oy, sy, q)) {
		*out_x = sx;
		*out_y = sy;
		return;
	}
	const int32_t bx = u_x11_reachable_round(ox, sx, (uint32_t)q);
	const int32_t by = u_x11_reachable_round(oy, sy, (uint32_t)q);
	const uint32_t n = u_x11_lattice_candidate_count(3);
	for (uint32_t k = 0; k < n; k++) {
		int32_t i = 0, j = 0;
		u_x11_lattice_candidate(k, &i, &j);
		int32_t rx = 0, ry = 0;
		dp_snap(ox, oy, bx + i * q, by + j * q, &rx, &ry);
		if (reachable(ox, rx, q) && reachable(oy, ry, q)) {
			*out_x = rx;
			*out_y = ry;
			return;
		}
	}
	*out_x = bx;
	*out_y = by;
}

//! The window manager: a request lands on the quantum lattice (rounded down,
//! as XWayland at 200 % was measured to: 5463 -> 5462).
int32_t
wm_place(int32_t v, int32_t q)
{
	const int32_t d = v;
	return q * (int32_t)std::floor((double)d / q);
}

struct Stats
{
	int moves = 0, misses = 0;
	double perp_max = 0.0, perp_sum2 = 0.0;
	double jump_max = 0.0; //!< largest sideways change between consecutive landings
	double phase_max = 0.0; //!< phase error of each landing vs the first landed position
};

/*!
 * One straight drag from (@p ox, @p oy) along (@p ux, @p uy), @p steps steps of
 * @p step px. @p use_picker: the new path, else the old one (snap(origin,
 * target) straight to XMoveWindow, never re-anchored).
 */
Stats
drag(int32_t ox, int32_t oy, double deg, int steps, double step, bool use_picker, int32_t q = 2)
{
	const double ux = std::cos(deg * M_PI / 180.0), uy = std::sin(deg * M_PI / 180.0);
	Stats s;
	D::X11Picker picker;
	picker.begin(ox, oy);
	int32_t at_x = ox, at_y = oy;         // where the window IS (server)
	int32_t asked_x = ox, asked_y = oy;   // last request
	bool have_ref = false;
	int32_t ref_x = 0, ref_y = 0;
	double last_perp = 0.0;
	bool have_last = false;
	for (int i = 1; i <= steps; i++) {
		const int32_t rx = ox + (int32_t)std::lround(ux * step * i);
		const int32_t ry = oy + (int32_t)std::lround(uy * step * i);
		int32_t sx = rx, sy = ry;
		if (use_picker) {
			const D::SnapMany snap = [&](const std::vector<D::Pt> &t, std::vector<D::Pt> *out) {
				out->resize(t.size());
				for (size_t k = 0; k < t.size(); k++) {
					rt_snap(picker.anchor_x(), picker.anchor_y(), t[k].x, t[k].y, q, &(*out)[k].x,
					        &(*out)[k].y);
				}
				return true;
			};
			const D::X11Picker::Result r = picker.pick(snap, rx, ry);
			sx = r.x;
			sy = r.y;
		} else {
			rt_snap(ox, oy, rx, ry, q, &sx, &sy);
		}
		if (sx == asked_x && sy == asked_y) {
			continue;
		}
		const int32_t before_x = at_x, before_y = at_y;
		at_x = wm_place(sx, q);
		at_y = wm_place(sy, q);
		asked_x = sx;
		asked_y = sy;
		s.moves++;
		if (at_x != sx || at_y != sy) {
			s.misses++;
		}
		if (use_picker && D::classify_landing(before_x, before_y, sx, sy, at_x, at_y) == D::Landing::Moved) {
			picker.reanchor(at_x, at_y);
		}
		// Sideways deviation of the LANDING from the drag line (through the
		// origin along the drag): what the eye sees.
		const double px = at_x - ox, py = at_y - oy;
		const double perp = py * ux - px * uy;
		s.perp_max = std::max(s.perp_max, std::fabs(perp));
		s.perp_sum2 += perp * perp;
		if (have_last) {
			s.jump_max = std::max(s.jump_max, std::fabs(perp - last_perp));
		}
		last_perp = perp;
		have_last = true;
		if (!have_ref) {
			have_ref = true;
			ref_x = at_x;
			ref_y = at_y;
		} else {
			s.phase_max = std::max(s.phase_max, phase_err(at_x - ref_x, at_y - ref_y));
		}
	}
	return s;
}

} // namespace model

static void
test_picker()
{
	std::printf("2. X11Picker at quantum 2 (modelled XWayland 200 %%)\n");
	struct Case
	{
		int32_t ox, oy;
		const char *what;
	};
	const Case origins[] = {{3856, 300, "even (settled) origin"}, {3857, 301, "odd (never placed) origin"}};
	const double degs[] = {0.0, 20.0, 45.0, 90.0};
	const double steps[] = {1.0, 6.0};
	for (const Case &c : origins) {
		for (double deg : degs) {
			for (double st : steps) {
				const model::Stats a = model::drag(c.ox, c.oy, deg, (int)(300 / st), st, false);
				const model::Stats b = model::drag(c.ox, c.oy, deg, (int)(300 / st), st, true);
				std::printf("   %-26s %4.0f deg step %.0f: misses %3d/%3d -> %d/%d, sideways max %.1f -> %.1f "
				            "rms %.2f -> %.2f, jump max %.1f -> %.1f, phase err after the first landing "
				            "%.3f -> %.3f\n",
				            c.what, deg, st, a.misses, a.moves, b.misses, b.moves, a.perp_max, b.perp_max,
				            a.moves ? std::sqrt(a.perp_sum2 / a.moves) : 0.0,
				            b.moves ? std::sqrt(b.perp_sum2 / b.moves) : 0.0, a.jump_max, b.jump_max,
				            a.phase_max, b.phase_max);
				// At most the first step of a drag from an unsettled origin
				// misses; from a settled one, none does.
				CHECK(b.misses <= ((c.ox & 1) ? 1 : 0));
				// Sideways: never worse than the old path, smaller on
				// average, and bounded (the reachable phase-correct
				// positions are sparse at quantum 2: the same bound the
				// Wayland choice reaches at 200 %, runtime#1748).
				CHECK(b.perp_max <= 5.5);
				CHECK(b.perp_max <= a.perp_max + 1e-9);
				CHECK(b.perp_sum2 / std::max(b.moves, 1) < a.perp_sum2 / std::max(a.moves, 1));
				// Every landing after the first is phase-correct relative to it
				// (the best phase within reach: the model's snap never does
				// better than ~0.2 of a period).
				CHECK(b.phase_max <= 0.25);
			}
		}
	}

	// No snap at all: the raw target, every time.
	{
		D::X11Picker p;
		p.begin(100, 100);
		const D::X11Picker::Result r = p.pick(D::SnapMany{}, 107, 103);
		CHECK(!r.snapped && r.x == 107 && r.y == 103);
		const D::SnapMany declines = [](const std::vector<D::Pt> &, std::vector<D::Pt> *) { return false; };
		D::X11Picker p2;
		p2.begin(100, 100);
		CHECK(!p2.pick(declines, 107, 103).snapped);
	}
	// An identity snap: the raw target itself is a candidate and wins.
	{
		D::X11Picker p;
		p.begin(100, 100);
		const D::SnapMany ident = [](const std::vector<D::Pt> &t, std::vector<D::Pt> *out) {
			*out = t;
			return true;
		};
		for (int i = 1; i <= 20; i++) {
			const D::X11Picker::Result r = p.pick(ident, 100 + i * 3, 100 + i);
			CHECK(r.snapped && r.x == 100 + i * 3 && r.y == 100 + i);
		}
	}
	// The memo: a drag step asks only about the new edge of the box.
	{
		D::X11Picker p;
		p.begin(0, 0);
		size_t asked = 0;
		const D::SnapMany ident = [&](const std::vector<D::Pt> &t, std::vector<D::Pt> *out) {
			asked += t.size();
			*out = t;
			return true;
		};
		const int32_t side = 2 * D::X11Picker::kRadius + 1;
		p.pick(ident, 0, 0);
		CHECK(asked == (size_t)(side * side));
		p.pick(ident, 1, 0);
		CHECK(asked == (size_t)(side * side + side));
		p.reanchor(5, 5); // answers are relative to the anchor: all asked again
		p.pick(ident, 1, 0);
		CHECK(asked == (size_t)(2 * side * side + side));
		CHECK(p.reanchors() == 1);
	}
}

/*
 * 3. classify_landing
 */
static void
test_landing()
{
	std::printf("3. classify_landing\n");
	CHECK(D::classify_landing(10, 10, 13, 11, 13, 11) == D::Landing::Exact);
	CHECK(D::classify_landing(10, 10, 13, 11, 10, 10) == D::Landing::Pending);
	CHECK(D::classify_landing(10, 10, 13, 11, 12, 10) == D::Landing::Moved);
	// A request for where the window already is: exact.
	CHECK(D::classify_landing(10, 10, 10, 10, 10, 10) == D::Landing::Exact);
}

/*
 * 4. ButtonDrag (X11)
 */
static void
test_button_drag()
{
	std::printf("4. ButtonDrag\n");
	const unsigned b3 = D::x11_button_mask(3), b1 = D::x11_button_mask(1);
	CHECK(b1 == (1u << 8) && b3 == (1u << 10) && D::x11_button_mask(8) == 0);
	{
		// Normal: press, motion with the button held, release.
		D::ButtonDrag d;
		CHECK(d.press(3) && d.active() && d.button() == 3);
		CHECK(!d.motion(b3));
		CHECK(!d.release(1)); // another button's release
		CHECK(d.active());
		CHECK(d.release(3) && !d.active() && d.last_end() == D::ButtonDrag::End::Release);
		CHECK(d.take_synthetic_release() == 0);
		CHECK(!d.swallow_release(3));
	}
	{
		// The release never arrives; the next motion says the button is up.
		D::ButtonDrag d;
		d.press(3);
		CHECK(d.motion(b1) && !d.active() && d.last_end() == D::ButtonDrag::End::MotionUp);
		CHECK(d.take_synthetic_release() == 3);
		CHECK(d.take_synthetic_release() == 0); // once
		CHECK(d.swallow_release(3));            // the late real one
		CHECK(!d.swallow_release(3));
	}
	{
		// ...or no motion at all: the pointer state queried once per pump.
		D::ButtonDrag d;
		d.press(3);
		CHECK(!d.poll(b3 | b1));
		CHECK(d.poll(0) && d.last_end() == D::ButtonDrag::End::PollUp);
		CHECK(d.take_synthetic_release() == 3);
		// A fresh press before the stale release came: that release is not
		// swallowed any more (it belongs to the new press).
		CHECK(d.press(3));
		CHECK(!d.swallow_release(3));
		CHECK(d.release(3));
	}
	{
		// A bar drag (button 1): the app never saw the press, so it is owed
		// nothing, and the late release is still swallowed.
		D::ButtonDrag d;
		CHECK(d.press(1, true) && d.from_bar());
		CHECK(d.poll(0));
		CHECK(d.take_synthetic_release() == 0);
		CHECK(d.swallow_release(1));
	}
	{
		// A second press during a drag never restarts it.
		D::ButtonDrag d;
		d.press(3);
		CHECK(!d.press(1));
		CHECK(d.button() == 3);
		CHECK(!d.motion(b3 | b1));
		CHECK(d.release(3));
	}
	{
		// Focus lost / cancelled.
		D::ButtonDrag d;
		d.press(3);
		CHECK(d.cancel(D::ButtonDrag::End::FocusLost) && d.last_end() == D::ButtonDrag::End::FocusLost);
		CHECK(!d.cancel());
		CHECK(d.take_synthetic_release() == 3);
	}
	{
		// A side button has no core mask: only its release ends it.
		D::ButtonDrag d;
		d.press(8);
		CHECK(!d.motion(0) && !d.poll(0) && d.active());
		CHECK(d.release(8));
	}
}

/*
 * 5. Wayland
 */
static void
test_wayland()
{
	std::printf("5. Wayland\n");
	using M = D::WlDragMode;
	const D::WlDragEnv mutter_new{true, true}, mutter_old{true, false}, other{false, false};
	CHECK(D::wl_drag_mode(1, mutter_new) == M::CompositorMove);
	CHECK(D::wl_drag_mode(1, mutter_old) == M::CompositorMove);
	CHECK(D::wl_drag_mode(3, mutter_new) == M::ShellPointer);
	CHECK(D::wl_drag_mode(2, mutter_new) == M::ShellPointer);
	CHECK(D::wl_drag_mode(3, mutter_old) == M::Nobody); // could never be ended
	CHECK(D::wl_drag_mode(8, mutter_new) == M::Nobody); // no Clutter mask
	CHECK(D::wl_drag_mode(3, other) == M::CompositorMove);
	CHECK(D::wl_drag_mode(8, other) == M::CompositorMove);
	{
		// Compositor move: leave at the grab, enter after it: the app is owed
		// the release it never saw.
		D::WlContentDrag d;
		d.begin(1, M::CompositorMove);
		CHECK(d.active() && !d.suppress_motion());
		D::WlContentDrag::Out o = d.on_leave();
		CHECK(!o.ended && d.active());
		o = d.on_enter();
		CHECK(o.ended && o.synth_release == 1 && !o.stop_shell && !d.active());
		o = d.on_enter();
		CHECK(!o.ended && o.synth_release == 0);
	}
	{
		// The compositor refused the move: the release reaches us.
		D::WlContentDrag d;
		d.begin(3, M::CompositorMove);
		D::WlContentDrag::Out o = d.on_release(3);
		CHECK(o.ended && o.synth_release == 0 && !d.active());
		CHECK(!d.on_enter().ended);
	}
	{
		// Shell pointer drag: the client keeps the implicit grab and sees the
		// release; the extension is told to stop.
		D::WlContentDrag d;
		d.begin(3, M::ShellPointer);
		CHECK(d.active() && d.suppress_motion());
		CHECK(!d.on_release(1).ended); // another button
		CHECK(!d.on_enter().ended);
		D::WlContentDrag::Out o = d.on_release(3);
		CHECK(o.ended && o.stop_shell && o.synth_release == 0 && !d.active() && !d.suppress_motion());
	}
	{
		// Shell pointer drag, the implicit grab lost (leave): stop, and owe
		// the release.
		D::WlContentDrag d;
		d.begin(3, M::ShellPointer);
		D::WlContentDrag::Out o = d.on_leave();
		CHECK(o.ended && o.stop_shell && o.synth_release == 3 && !d.active());
		CHECK(!d.on_release(3).ended);
	}
	{
		D::WlContentDrag d;
		d.begin(3, M::Nobody);
		CHECK(!d.active() && !d.on_release(3).ended && !d.on_enter().ended);
	}
}

int
main()
{
	test_choice();
	test_picker();
	test_landing();
	test_button_drag();
	test_wayland();
	if (g_fail != 0) {
		std::fprintf(stderr, "%d check(s) FAILED\n", g_fail);
		return 1;
	}
	std::printf("all drag checks passed\n");
	return 0;
}

// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XDND (version 5) drop target for DxrLinuxWindow's X11 leg: files
 *         dropped on the window arrive as a Drop event.
 *
 * Receive only, `text/uri-list` only, copy action only. The top-level
 * carries XdndAware; a source then talks to it in ClientMessages
 * (XdndEnter / XdndPosition / XdndLeave / XdndDrop), and on the drop the
 * payload is fetched as the XdndSelection, which the server answers with a
 * SelectionNotify. Nothing here waits on the source: every step is an event
 * the pump already drains. A source that never answers the selection
 * request is given up on after a few seconds (tick()), with XdndFinished
 * sent as "not accepted", so it can end its own drag.
 */
#pragma once

#include <X11/Xlib.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

class DxrX11Dnd
{
public:
	//! Advertise XdndAware on @p toplevel. Call once, after the window exists.
	void
	init(Display *dpy, ::Window toplevel);

	/*!
	 * Offer an event. True when it was XDND traffic (consumed). On a
	 * completed drop, @p paths is filled and @p root_x / @p root_y hold the
	 * last drop position in ROOT coordinates; the return is still true.
	 *
	 * @param[out] dropped set when a drop completed with at least one path
	 */
	bool
	handle(const XEvent &ev, bool *dropped, std::vector<std::string> *paths, int *root_x, int *root_y);

	//! Give up on a drop whose selection never arrived (call once per pump).
	void
	tick();

private:
	void
	send_status(bool accept);
	void
	send_finished(bool accepted);
	void
	reset();

	Display *m_dpy = nullptr;
	::Window m_win = 0;

	Atom m_aware = 0, m_enter = 0, m_position = 0, m_status = 0, m_leave = 0, m_drop = 0, m_finished = 0;
	Atom m_selection = 0, m_type_list = 0, m_action_copy = 0, m_uri_list = 0, m_property = 0;

	::Window m_source = 0;     //!< the source of the drag over us now
	int m_version = 0;         //!< its XDND version
	bool m_accept = false;     //!< it offers text/uri-list
	int m_root_x = 0, m_root_y = 0;
	bool m_waiting = false;    //!< a drop is waiting for its SelectionNotify
	std::chrono::steady_clock::time_point m_wait_since{};
};

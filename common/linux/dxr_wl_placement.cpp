// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  libdbus client of the compositor's drag lattice. See the header.
 */

#include "dxr_wl_placement.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef DXR_APP_HAVE_DBUS
#include <dbus/dbus.h>
#include <unistd.h> // getpid, to pick our own DragLatticeNeeded out of the stream
#endif

#define WLP_BUS "org.displayxr.WindowGeometry"
#define WLP_PATH "/org/displayxr/WindowPlacement"
#define WLP_IFACE "org.displayxr.WindowPlacement1"
#define WLP_NEEDED_MATCH "type='signal',interface='" WLP_IFACE "',member='DragLatticeNeeded'"
#define WLP_DONE_MATCH "type='signal',interface='" WLP_IFACE "',member='DragLatticeDone'"
//! GetPlacementCapabilities bit: the publisher can constrain a drag.
#define WLP_CAP_DRAG_LATTICE 1u
//! GetPlacementCapabilities bit: SetDragLatticeAt (an explicit start; v8).
#define WLP_CAP_EXPLICIT_START 2u

DxrWlPlacement::~DxrWlPlacement()
{
	disconnect();
}

#ifdef DXR_APP_HAVE_DBUS

bool
DxrWlPlacement::connect()
{
	if (m_conn != nullptr) {
		return m_lattice;
	}
	DBusError err;
	dbus_error_init(&err);
	// Private connection: the app owns its lifetime, and a shared one must
	// never be closed.
	DBusConnection *conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
	if (conn == nullptr) {
		m_why = "no session bus";
		dbus_error_free(&err);
		return false;
	}
	dbus_connection_set_exit_on_disconnect(conn, FALSE);
	m_conn = conn;

	dbus_bus_add_match(conn, WLP_NEEDED_MATCH, &err);
	if (dbus_error_is_set(&err)) {
		dbus_error_free(&err);
	}
	dbus_bus_add_match(conn, WLP_DONE_MATCH, &err); // extension v7+; harmless before
	if (dbus_error_is_set(&err)) {
		dbus_error_free(&err);
	}
	dbus_connection_flush(conn);

	// One bounded probe at start-up, so a press never has to find out.
	DBusMessage *call = dbus_message_new_method_call(WLP_BUS, WLP_PATH, WLP_IFACE, "GetPlacementCapabilities");
	if (call == nullptr) {
		m_why = "out of memory";
		return false;
	}
	DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, call, 200, &err);
	dbus_message_unref(call);
	if (reply == nullptr) {
		m_why = "the geometry extension is older than version 6 (no drag lattice) — the title bar drags "
		        "through the compositor unsnapped";
		dbus_error_free(&err);
		return false;
	}
	dbus_uint32_t caps = 0;
	dbus_message_get_args(reply, nullptr, DBUS_TYPE_UINT32, &caps, DBUS_TYPE_INVALID);
	dbus_message_unref(reply);
	m_lattice = (caps & WLP_CAP_DRAG_LATTICE) != 0;
	m_explicit_start = (caps & WLP_CAP_EXPLICIT_START) != 0;
	m_why = m_lattice ? "ready — the compositor can constrain a drag to the interlace lattice"
	                  : "the compositor has no Meta.ExternalConstraint, so a drag cannot be constrained — the "
	                    "title bar drags unsnapped";
	return m_lattice;
}

bool
DxrWlPlacement::set_drag_lattice(bool extend,
                                 int32_t cell,
                                 int32_t min_dx,
                                 int32_t min_dy,
                                 int32_t max_dx,
                                 int32_t max_dy,
                                 const std::vector<int32_t> &dx,
                                 const std::vector<int32_t> &dy,
                                 int32_t *start_x,
                                 int32_t *start_y,
                                 const int32_t *explicit_start)
{
	if (!m_lattice || m_conn == nullptr || dx.size() != dy.size()) {
		return false;
	}
	const bool at = explicit_start != nullptr && m_explicit_start;
	DBusMessage *call =
	    dbus_message_new_method_call(WLP_BUS, WLP_PATH, WLP_IFACE, at ? "SetDragLatticeAt" : "SetDragLattice");
	if (call == nullptr) {
		return false;
	}
	dbus_uint32_t pid = 0; // 0 = "me"; the publisher takes the PID from the bus
	dbus_bool_t ext = extend ? TRUE : FALSE;
	dbus_int32_t c = cell, a = min_dx, b = min_dy, e = max_dx, f = max_dy;
	const dbus_int32_t *px = dx.data(), *py = dy.data();
	const int n = (int)dx.size();
	if (at) {
		dbus_int32_t s0 = explicit_start[0], s1 = explicit_start[1];
		dbus_message_append_args(call, DBUS_TYPE_UINT32, &pid, DBUS_TYPE_INT32, &s0, DBUS_TYPE_INT32, &s1,
		                         DBUS_TYPE_INVALID);
	} else {
		dbus_message_append_args(call, DBUS_TYPE_UINT32, &pid, DBUS_TYPE_INVALID);
	}
	dbus_message_append_args(call, DBUS_TYPE_BOOLEAN, &ext, DBUS_TYPE_INT32, &c, DBUS_TYPE_INT32, &a,
	                         DBUS_TYPE_INT32, &b, DBUS_TYPE_INT32, &e, DBUS_TYPE_INT32, &f, DBUS_TYPE_ARRAY,
	                         DBUS_TYPE_INT32, &px, n, DBUS_TYPE_ARRAY, DBUS_TYPE_INT32, &py, n, DBUS_TYPE_INVALID);
	// Bounded and blocking: it runs on the window thread when a worker's
	// probe is collected — at the press that is already after
	// xdg_toplevel.move went out (the grab never waits for the table; the
	// publisher passes moves through until it lands, and the explicit start
	// anchors a late table at the press position).
	DBusMessage *reply = dbus_connection_send_with_reply_and_block((DBusConnection *)m_conn, call, 100, nullptr);
	dbus_message_unref(call);
	if (reply == nullptr) {
		return false;
	}
	dbus_bool_t ok = FALSE;
	dbus_int32_t sx = 0, sy = 0;
	dbus_message_get_args(reply, nullptr, DBUS_TYPE_BOOLEAN, &ok, DBUS_TYPE_INT32, &sx, DBUS_TYPE_INT32, &sy,
	                      DBUS_TYPE_INVALID);
	dbus_message_unref(reply);
	if (start_x != nullptr) {
		*start_x = (int32_t)sx;
	}
	if (start_y != nullptr) {
		*start_y = (int32_t)sy;
	}
	return ok == TRUE;
}

bool
DxrWlPlacement::move_window(int32_t x, int32_t y)
{
	if (m_conn == nullptr) {
		return false;
	}
	DBusMessage *call = dbus_message_new_method_call(WLP_BUS, WLP_PATH, WLP_IFACE, "MoveWindow");
	if (call == nullptr) {
		return false;
	}
	dbus_uint32_t pid = 0;
	dbus_int32_t ax = x, ay = y;
	dbus_message_append_args(call, DBUS_TYPE_UINT32, &pid, DBUS_TYPE_INT32, &ax, DBUS_TYPE_INT32, &ay,
	                         DBUS_TYPE_INVALID);
	DBusMessage *reply = dbus_connection_send_with_reply_and_block((DBusConnection *)m_conn, call, 200, nullptr);
	dbus_message_unref(call);
	if (reply == nullptr) {
		return false;
	}
	dbus_bool_t moved = FALSE;
	dbus_message_get_args(reply, nullptr, DBUS_TYPE_BOOLEAN, &moved, DBUS_TYPE_INVALID);
	dbus_message_unref(reply);
	return moved == TRUE;
}

bool
DxrWlPlacement::poll_needed(int32_t *dx, int32_t *dy)
{
	if (m_conn == nullptr) {
		return false;
	}
	DBusConnection *conn = (DBusConnection *)m_conn;
	dbus_connection_read_write(conn, 0);
	bool got = false;
	DBusMessage *msg = nullptr;
	while ((msg = dbus_connection_pop_message(conn)) != nullptr) {
		if (dbus_message_is_signal(msg, WLP_IFACE, "DragLatticeNeeded")) {
			dbus_uint32_t pid = 0;
			dbus_int32_t x = 0, y = 0;
			if (dbus_message_get_args(msg, nullptr, DBUS_TYPE_UINT32, &pid, DBUS_TYPE_INT32, &x,
			                          DBUS_TYPE_INT32, &y, DBUS_TYPE_INVALID) &&
			    (int32_t)pid == (int32_t)getpid()) {
				*dx = (int32_t)x; // the newest request wins
				*dy = (int32_t)y;
				got = true;
			}
		} else if (dbus_message_is_signal(msg, WLP_IFACE, "DragLatticeDone")) {
			dbus_uint32_t pid = 0, moves = 0, corrected = 0, misses = 0, maxc = 0, tables = 0;
			dbus_bool_t landed = FALSE;
			if (dbus_message_get_args(msg, nullptr, DBUS_TYPE_UINT32, &pid, DBUS_TYPE_UINT32, &moves,
			                          DBUS_TYPE_UINT32, &corrected, DBUS_TYPE_UINT32, &misses,
			                          DBUS_TYPE_UINT32, &maxc, DBUS_TYPE_UINT32, &tables, DBUS_TYPE_BOOLEAN,
			                          &landed, DBUS_TYPE_INVALID) &&
			    (int32_t)pid == (int32_t)getpid()) {
				m_done.moves = moves;
				m_done.corrected = corrected;
				m_done.misses = misses;
				m_done.max_correction = maxc;
				m_done.tables = tables;
				m_done.landed_on_table = landed == TRUE;
				m_have_done = true;
			}
		}
		dbus_message_unref(msg);
	}
	return got;
}

namespace {
//! The integers of a JSON array `"key":[a,b,c,d]` inside [from, to).
bool
json_int4(const char *from, const char *to, const char *key, int32_t out[4])
{
	const char *p = strstr(from, key);
	if (p == nullptr || p >= to) {
		return false;
	}
	p = strchr(p, '[');
	if (p == nullptr || p >= to) {
		return false;
	}
	p++;
	for (int i = 0; i < 4; i++) {
		char *end = nullptr;
		const long v = strtol(p, &end, 10);
		if (end == p) {
			return false;
		}
		out[i] = (int32_t)v;
		p = end;
		while (*p == ',' || *p == ' ') {
			p++;
		}
	}
	return true;
}

//! A number `"key":v` inside [from, to).
bool
json_num(const char *from, const char *to, const char *key, double *out)
{
	const char *p = strstr(from, key);
	if (p == nullptr || p >= to) {
		return false;
	}
	p += strlen(key);
	char *end = nullptr;
	const double v = strtod(p, &end);
	if (end == p) {
		return false;
	}
	*out = v;
	return true;
}
} // namespace

bool
DxrWlPlacement::get_own_geometry(OwnGeometry *out)
{
	if (m_conn == nullptr || out == nullptr) {
		return false;
	}
	DBusMessage *call = dbus_message_new_method_call(WLP_BUS, "/org/displayxr/WindowGeometry",
	                                                 "org.displayxr.WindowGeometry1", "GetWindows");
	if (call == nullptr) {
		return false;
	}
	DBusMessage *reply = dbus_connection_send_with_reply_and_block((DBusConnection *)m_conn, call, 100, nullptr);
	dbus_message_unref(call);
	if (reply == nullptr) {
		return false;
	}
	const char *json = nullptr;
	bool ok = false;
	if (dbus_message_get_args(reply, nullptr, DBUS_TYPE_STRING, &json, DBUS_TYPE_INVALID) && json != nullptr) {
		// The publisher's own JSON.stringify output: one object per window,
		// each starting at its "pid". Ours is the first with our PID.
		char needle[32];
		snprintf(needle, sizeof(needle), "\"pid\":%d,", (int)getpid());
		const char *obj = strstr(json, needle);
		if (obj != nullptr) {
			const char *next = strstr(obj + 1, "\"pid\":");
			const char *end = next != nullptr ? next : json + strlen(json);
			const char *mon = strstr(obj, "\"monitor\":{");
			// The monitor object's keys, in its own order: x, y, w, h, scale.
			double mx = 0.0, my = 0.0, mw = 0.0, mh = 0.0;
			ok = json_int4(obj, end, "\"frame\":", out->frame) && json_int4(obj, end, "\"buffer\":", out->buffer) &&
			     mon != nullptr && mon < end && json_num(mon, end, "\"x\":", &mx) &&
			     json_num(mon, end, "\"y\":", &my) && json_num(mon, end, "\"w\":", &mw) &&
			     json_num(mon, end, "\"h\":", &mh) && json_num(mon, end, "\"scale\":", &out->monitor_scale) &&
			     out->monitor_scale > 0.0;
			out->monitor[0] = (int32_t)mx;
			out->monitor[1] = (int32_t)my;
			out->monitor[2] = (int32_t)mw;
			out->monitor[3] = (int32_t)mh;
		}
	}
	dbus_message_unref(reply);
	return ok;
}

bool
DxrWlPlacement::take_done(DragDone *out)
{
	if (!m_have_done) {
		return false;
	}
	m_have_done = false;
	*out = m_done;
	return true;
}

void
DxrWlPlacement::clear_drag_lattice()
{
	if (m_conn == nullptr) {
		return;
	}
	DBusMessage *call = dbus_message_new_method_call(WLP_BUS, WLP_PATH, WLP_IFACE, "ClearDragLattice");
	if (call == nullptr) {
		return;
	}
	dbus_uint32_t pid = 0; // 0 = the caller
	dbus_message_append_args(call, DBUS_TYPE_UINT32, &pid, DBUS_TYPE_INVALID);
	dbus_message_set_no_reply(call, TRUE); // fire and forget: never wait on the shell mid-drag
	dbus_connection_send((DBusConnection *)m_conn, call, nullptr);
	dbus_connection_flush((DBusConnection *)m_conn);
	dbus_message_unref(call);
}

void
DxrWlPlacement::disconnect()
{
	if (m_conn != nullptr) {
		dbus_connection_close((DBusConnection *)m_conn);
		dbus_connection_unref((DBusConnection *)m_conn);
		m_conn = nullptr;
	}
	m_lattice = false;
}

#else // !DXR_APP_HAVE_DBUS

bool
DxrWlPlacement::connect()
{
	m_why = "this build has no libdbus (install libdbus-1-dev and reconfigure)";
	return false;
}

bool
DxrWlPlacement::set_drag_lattice(bool extend,
                                 int32_t cell,
                                 int32_t min_dx,
                                 int32_t min_dy,
                                 int32_t max_dx,
                                 int32_t max_dy,
                                 const std::vector<int32_t> &dx,
                                 const std::vector<int32_t> &dy,
                                 int32_t *start_x,
                                 int32_t *start_y,
                                 const int32_t *explicit_start)
{
	(void)explicit_start;
	(void)start_x;
	(void)start_y;
	(void)extend;
	(void)cell;
	(void)min_dx;
	(void)min_dy;
	(void)max_dx;
	(void)max_dy;
	(void)dx;
	(void)dy;
	return false;
}

bool
DxrWlPlacement::poll_needed(int32_t *dx, int32_t *dy)
{
	(void)dx;
	(void)dy;
	return false;
}

bool
DxrWlPlacement::move_window(int32_t x, int32_t y)
{
	(void)x;
	(void)y;
	return false;
}

bool
DxrWlPlacement::take_done(DragDone *out)
{
	(void)out;
	return false;
}

bool
DxrWlPlacement::get_own_geometry(OwnGeometry *out)
{
	(void)out;
	return false;
}

void
DxrWlPlacement::clear_drag_lattice()
{
}

void
DxrWlPlacement::disconnect()
{
}

#endif // DXR_APP_HAVE_DBUS

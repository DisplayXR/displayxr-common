// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XDND drop target — see dxr_x11_dnd.h.
 */
#include "dxr_x11_dnd.h"

#include "dxr_input_text.h"

#include <X11/Xatom.h>

#include <cstdio>

namespace {
//! Payload cap: a uri-list of thousands of files is a few hundred KiB.
constexpr long kMaxBytes = 1L << 20;
//! A source that has not answered the selection request by now never will.
constexpr int kSelectionTimeoutMs = 3000;
} // namespace

void
DxrX11Dnd::init(Display *dpy, ::Window toplevel)
{
	m_dpy = dpy;
	m_win = toplevel;
	m_aware = XInternAtom(dpy, "XdndAware", False);
	m_enter = XInternAtom(dpy, "XdndEnter", False);
	m_position = XInternAtom(dpy, "XdndPosition", False);
	m_status = XInternAtom(dpy, "XdndStatus", False);
	m_leave = XInternAtom(dpy, "XdndLeave", False);
	m_drop = XInternAtom(dpy, "XdndDrop", False);
	m_finished = XInternAtom(dpy, "XdndFinished", False);
	m_selection = XInternAtom(dpy, "XdndSelection", False);
	m_type_list = XInternAtom(dpy, "XdndTypeList", False);
	m_action_copy = XInternAtom(dpy, "XdndActionCopy", False);
	m_uri_list = XInternAtom(dpy, "text/uri-list", False);
	m_property = XInternAtom(dpy, "DXR_XDND_DATA", False);
	const unsigned long version = 5;
	XChangeProperty(dpy, toplevel, m_aware, XA_ATOM, 32, PropModeReplace, (const unsigned char *)&version, 1);
}

void
DxrX11Dnd::reset()
{
	m_source = 0;
	m_version = 0;
	m_accept = false;
	m_waiting = false;
}

void
DxrX11Dnd::send_status(bool accept)
{
	XEvent r = {};
	r.xclient.type = ClientMessage;
	r.xclient.display = m_dpy;
	r.xclient.window = m_source;
	r.xclient.message_type = m_status;
	r.xclient.format = 32;
	r.xclient.data.l[0] = (long)m_win;
	r.xclient.data.l[1] = accept ? 1 : 0; // bit 0: will accept; bit 1 clear: empty no-resend rect
	r.xclient.data.l[2] = 0;
	r.xclient.data.l[3] = 0;
	r.xclient.data.l[4] = accept ? (long)m_action_copy : 0;
	XSendEvent(m_dpy, m_source, False, NoEventMask, &r);
	XFlush(m_dpy);
}

void
DxrX11Dnd::send_finished(bool accepted)
{
	if (m_source == 0) {
		return;
	}
	XEvent r = {};
	r.xclient.type = ClientMessage;
	r.xclient.display = m_dpy;
	r.xclient.window = m_source;
	r.xclient.message_type = m_finished;
	r.xclient.format = 32;
	r.xclient.data.l[0] = (long)m_win;
	r.xclient.data.l[1] = accepted ? 1 : 0;
	r.xclient.data.l[2] = accepted ? (long)m_action_copy : 0;
	XSendEvent(m_dpy, m_source, False, NoEventMask, &r);
	XFlush(m_dpy);
}

bool
DxrX11Dnd::handle(const XEvent &ev, bool *dropped, std::vector<std::string> *paths, int *root_x, int *root_y)
{
	*dropped = false;
	if (m_dpy == nullptr) {
		return false;
	}

	if (ev.type == SelectionNotify) {
		if (ev.xselection.selection != m_selection || !m_waiting) {
			return false;
		}
		m_waiting = false;
		bool ok = false;
		if (ev.xselection.property != None) {
			Atom type = None;
			int format = 0;
			unsigned long n = 0, after = 0;
			unsigned char *data = nullptr;
			if (XGetWindowProperty(m_dpy, m_win, ev.xselection.property, 0, kMaxBytes / 4, True, AnyPropertyType,
			                       &type, &format, &n, &after, &data) == Success &&
			    data != nullptr) {
				if (format == 8 && after == 0) {
					*paths = dxr_input::parse_uri_list(reinterpret_cast<const char *>(data), (size_t)n);
					ok = !paths->empty();
					*dropped = ok;
					*root_x = m_root_x;
					*root_y = m_root_y;
				} else {
					fprintf(stderr, "[WARN]  drop: X11 selection not taken (format %d, %lu bytes over "
					                "the cap)\n",
					        format, after);
				}
				XFree(data);
			}
		}
		send_finished(ok);
		reset();
		return true;
	}

	if (ev.type != ClientMessage) {
		return false;
	}
	const XClientMessageEvent &cm = ev.xclient;
	if (cm.message_type == m_enter) {
		reset();
		m_source = (::Window)cm.data.l[0];
		m_version = (int)((unsigned long)cm.data.l[1] >> 24);
		if ((cm.data.l[1] & 1) != 0) {
			// More than three types: they are in the source's XdndTypeList.
			Atom type = None;
			int format = 0;
			unsigned long n = 0, after = 0;
			unsigned char *data = nullptr;
			if (XGetWindowProperty(m_dpy, m_source, m_type_list, 0, 1024, False, XA_ATOM, &type, &format, &n,
			                       &after, &data) == Success &&
			    data != nullptr) {
				const Atom *atoms = reinterpret_cast<const Atom *>(data);
				for (unsigned long i = 0; i < n; i++) {
					m_accept = m_accept || atoms[i] == m_uri_list;
				}
				XFree(data);
			}
		} else {
			for (int i = 2; i <= 4; i++) {
				m_accept = m_accept || (Atom)cm.data.l[i] == m_uri_list;
			}
		}
		return true;
	}
	if (cm.message_type == m_position) {
		if (m_source == 0) {
			m_source = (::Window)cm.data.l[0];
		}
		m_root_x = (int)(((unsigned long)cm.data.l[2] >> 16) & 0xFFFF);
		m_root_y = (int)((unsigned long)cm.data.l[2] & 0xFFFF);
		send_status(m_accept);
		return true;
	}
	if (cm.message_type == m_leave) {
		reset();
		return true;
	}
	if (cm.message_type == m_drop) {
		if (m_source == 0) {
			m_source = (::Window)cm.data.l[0];
		}
		if (!m_accept) {
			send_finished(false);
			reset();
			return true;
		}
		const Time t = m_version >= 1 ? (Time)cm.data.l[2] : CurrentTime;
		XConvertSelection(m_dpy, m_selection, m_uri_list, m_property, m_win, t);
		XFlush(m_dpy);
		m_waiting = true;
		m_wait_since = std::chrono::steady_clock::now();
		return true;
	}
	return false;
}

void
DxrX11Dnd::tick()
{
	if (!m_waiting) {
		return;
	}
	const auto ms =
	    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_wait_since).count();
	if (ms > kSelectionTimeoutMs) {
		fprintf(stderr, "[WARN]  drop: the X11 source never delivered the dropped data — ignored\n");
		send_finished(false);
		reset();
	}
}

// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Wayland drop target — see dxr_wl_dnd.h.
 */
#include "dxr_wl_dnd.h"

#include "dxr_input_text.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

namespace {
const char *const kUriList = "text/uri-list";
//! A uri-list of thousands of files is a few hundred KiB.
constexpr size_t kMaxBytes = 1u << 20;
//! A source that has not finished writing by now is not going to.
constexpr int kMaxMs = 3000;
} // namespace

DxrWlDnd::~DxrWlDnd()
{
	destroy();
}

bool
DxrWlDnd::on_global(struct wl_registry *r, uint32_t name, const char *iface, uint32_t version)
{
	if (strcmp(iface, wl_data_device_manager_interface.name) != 0) {
		return false;
	}
	m_manager_version = version < 3 ? version : 3;
	m_manager = static_cast<struct wl_data_device_manager *>(
	    wl_registry_bind(r, name, &wl_data_device_manager_interface, m_manager_version));
	return true;
}

void
DxrWlDnd::attach(struct wl_display *display, struct wl_seat *seat, struct wl_surface *content)
{
	m_display = display;
	m_content = content;
	if (m_device != nullptr || m_manager == nullptr || seat == nullptr) {
		return;
	}
	m_device = wl_data_device_manager_get_data_device(m_manager, seat);
	static const struct wl_data_device_listener kListener = {
	    s_data_offer, s_enter, s_leave, s_motion, s_drop, s_selection,
	};
	wl_data_device_add_listener(m_device, &kListener, this);
}

DxrWlDnd::Offer *
DxrWlDnd::find(struct wl_data_offer *o)
{
	for (auto &e : m_offers) {
		if (e.offer == o) {
			return &e;
		}
	}
	return nullptr;
}

void
DxrWlDnd::drop_offer(struct wl_data_offer *o)
{
	if (o == nullptr) {
		return;
	}
	for (auto it = m_offers.begin(); it != m_offers.end(); ++it) {
		if (it->offer == o) {
			m_offers.erase(it);
			break;
		}
	}
	wl_data_offer_destroy(o);
}

void
DxrWlDnd::s_data_offer(void *data, struct wl_data_device *d, struct wl_data_offer *o)
{
	(void)d;
	auto *self = static_cast<DxrWlDnd *>(data);
	Offer e;
	e.offer = o;
	self->m_offers.push_back(e);
	static const struct wl_data_offer_listener kOfferListener = {
	    s_offer_offer,
	    s_offer_source_actions,
	    s_offer_action,
	};
	wl_data_offer_add_listener(o, &kOfferListener, self);
}

void
DxrWlDnd::s_offer_offer(void *data, struct wl_data_offer *o, const char *mime)
{
	auto *self = static_cast<DxrWlDnd *>(data);
	Offer *e = self->find(o);
	if (e != nullptr && mime != nullptr && strcmp(mime, kUriList) == 0) {
		e->uri_list = true;
	}
}

void
DxrWlDnd::s_offer_source_actions(void *data, struct wl_data_offer *o, uint32_t actions)
{
	(void)data;
	(void)o;
	(void)actions;
}

void
DxrWlDnd::s_offer_action(void *data, struct wl_data_offer *o, uint32_t action)
{
	(void)data;
	(void)o;
	(void)action;
}

void
DxrWlDnd::s_enter(void *data,
                  struct wl_data_device *d,
                  uint32_t serial,
                  struct wl_surface *s,
                  wl_fixed_t x,
                  wl_fixed_t y,
                  struct wl_data_offer *o)
{
	(void)d;
	auto *self = static_cast<DxrWlDnd *>(data);
	// A previous drag that left without a drop, not yet cleaned up.
	if (self->m_current != nullptr && self->m_current != o && self->m_current != self->m_receiving) {
		self->drop_offer(self->m_current);
	}
	self->m_current = o;
	self->m_x = wl_fixed_to_double(x);
	self->m_y = wl_fixed_to_double(y);
	const Offer *e = o != nullptr ? self->find(o) : nullptr;
	// The content surface only: the title bar is chrome, not a drop target.
	self->m_current_ok = e != nullptr && e->uri_list && s != nullptr && s == self->m_content;
	if (o == nullptr) {
		return;
	}
	if (self->m_current_ok) {
		wl_data_offer_accept(o, serial, kUriList);
		if (self->m_manager_version >= 3) {
			wl_data_offer_set_actions(o, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
			                          WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
		}
	} else {
		wl_data_offer_accept(o, serial, nullptr);
		if (self->m_manager_version >= 3) {
			wl_data_offer_set_actions(o, WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE,
			                          WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE);
		}
	}
}

void
DxrWlDnd::s_leave(void *data, struct wl_data_device *d)
{
	(void)d;
	auto *self = static_cast<DxrWlDnd *>(data);
	// After a drop, leave follows too: the offer then belongs to the transfer.
	if (self->m_current != nullptr && self->m_current != self->m_receiving) {
		self->drop_offer(self->m_current);
	}
	self->m_current = nullptr;
	self->m_current_ok = false;
}

void
DxrWlDnd::s_motion(void *data, struct wl_data_device *d, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	(void)d;
	(void)time;
	auto *self = static_cast<DxrWlDnd *>(data);
	self->m_x = wl_fixed_to_double(x);
	self->m_y = wl_fixed_to_double(y);
}

void
DxrWlDnd::s_drop(void *data, struct wl_data_device *d)
{
	(void)d;
	auto *self = static_cast<DxrWlDnd *>(data);
	struct wl_data_offer *o = self->m_current;
	if (o == nullptr || !self->m_current_ok) {
		return;
	}
	if (self->m_receiving != nullptr) {
		// One transfer at a time; a second drop while the first still reads
		// is refused rather than queued (it would be a second drag in < 3 s).
		fprintf(stderr, "[WARN]  drop: a previous drop is still being read — this one is ignored\n");
		return;
	}
	int fds[2] = {-1, -1};
	if (pipe2(fds, O_CLOEXEC) != 0) {
		fprintf(stderr, "[WARN]  drop: pipe2 failed — drop ignored\n");
		return;
	}
	wl_data_offer_receive(o, kUriList, fds[1]);
	close(fds[1]); // the source has its own copy once the request is flushed
	if (self->m_display != nullptr) {
		wl_display_flush(self->m_display);
	}
	self->m_receiving = o;
	self->m_drop_x = self->m_x;
	self->m_drop_y = self->m_y;
	self->m_drain.start(fds[0], kMaxBytes, kMaxMs);
}

void
DxrWlDnd::s_selection(void *data, struct wl_data_device *d, struct wl_data_offer *o)
{
	(void)d;
	// The clipboard: not used. Its offer is ours to destroy.
	auto *self = static_cast<DxrWlDnd *>(data);
	if (o != nullptr && o != self->m_current && o != self->m_receiving) {
		self->drop_offer(o);
	}
}

bool
DxrWlDnd::pump(std::vector<std::string> *paths, double *x, double *y)
{
	if (m_receiving == nullptr) {
		return false;
	}
	const dxr_input::FdDrain::State st = m_drain.step();
	if (st == dxr_input::FdDrain::State::Reading) {
		return false;
	}
	bool ok = false;
	if (st == dxr_input::FdDrain::State::Done) {
		*paths = dxr_input::parse_uri_list(m_drain.data().data(), m_drain.data().size());
		ok = !paths->empty();
		*x = m_drop_x;
		*y = m_drop_y;
	} else {
		fprintf(stderr, "[WARN]  drop: reading the dropped data failed (%s) — drop ignored\n",
		        m_drain.why() != nullptr ? m_drain.why() : "?");
	}
	if (m_manager_version >= 3 && ok) {
		wl_data_offer_finish(m_receiving);
	}
	struct wl_data_offer *o = m_receiving;
	m_receiving = nullptr;
	if (m_current == o) {
		m_current = nullptr;
		m_current_ok = false;
	}
	drop_offer(o);
	m_drain.reset();
	return ok;
}

void
DxrWlDnd::destroy()
{
	m_drain.reset();
	for (auto &e : m_offers) {
		if (e.offer != nullptr) {
			wl_data_offer_destroy(e.offer);
		}
	}
	m_offers.clear();
	m_current = nullptr;
	m_receiving = nullptr;
	if (m_device != nullptr) {
		if (m_manager_version >= 2) {
			wl_data_device_release(m_device);
		} else {
			wl_data_device_destroy(m_device);
		}
		m_device = nullptr;
	}
	if (m_manager != nullptr) {
		wl_data_device_manager_destroy(m_manager);
		m_manager = nullptr;
	}
	m_content = nullptr;
	m_display = nullptr;
}

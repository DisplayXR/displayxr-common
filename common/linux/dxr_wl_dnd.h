// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Wayland drop target for DxrLinuxWindow: files dropped on the
 *         content surface arrive as a Drop event.
 *
 * wl_data_device (v3 when offered), receive only, `text/uri-list` only,
 * copy action. On the drop the client asks for the payload
 * (wl_data_offer.receive) and the SOURCE writes it into a pipe the client
 * reads. That read must never stall a frame — a slow or wedged source would
 * freeze the window — so the pipe is non-blocking and drained a little per
 * pump() (dxr_fd_drain.h), with a size cap and a time cap; the offer is
 * finished and destroyed when the transfer ends either way.
 */
#pragma once

#include "dxr_fd_drain.h"

#include <wayland-client.h>

#include <cstdint>
#include <string>
#include <vector>

class DxrWlDnd
{
public:
	DxrWlDnd() = default;
	~DxrWlDnd();
	DxrWlDnd(const DxrWlDnd &) = delete;
	DxrWlDnd &
	operator=(const DxrWlDnd &) = delete;

	//! Registry hook. True when the global was ours (bound).
	bool
	on_global(struct wl_registry *r, uint32_t name, const char *iface, uint32_t version);

	//! Start receiving drops on @p content (needs the seat and the manager).
	//! Idempotent; a no-op when either is missing.
	void
	attach(struct wl_display *display, struct wl_seat *seat, struct wl_surface *content);

	/*!
	 * Advance a transfer in flight. Never blocks. True when a drop completed
	 * with at least one path: @p paths filled, @p x / @p y the drop point in
	 * surface-local LOGICAL px.
	 */
	bool
	pump(std::vector<std::string> *paths, double *x, double *y);

	void
	destroy();

private:
	struct Offer
	{
		struct wl_data_offer *offer = nullptr;
		bool uri_list = false;
	};

	Offer *
	find(struct wl_data_offer *o);
	void
	drop_offer(struct wl_data_offer *o);

	static void
	s_data_offer(void *data, struct wl_data_device *d, struct wl_data_offer *o);
	static void
	s_enter(void *data,
	        struct wl_data_device *d,
	        uint32_t serial,
	        struct wl_surface *s,
	        wl_fixed_t x,
	        wl_fixed_t y,
	        struct wl_data_offer *o);
	static void
	s_leave(void *data, struct wl_data_device *d);
	static void
	s_motion(void *data, struct wl_data_device *d, uint32_t time, wl_fixed_t x, wl_fixed_t y);
	static void
	s_drop(void *data, struct wl_data_device *d);
	static void
	s_selection(void *data, struct wl_data_device *d, struct wl_data_offer *o);
	static void
	s_offer_offer(void *data, struct wl_data_offer *o, const char *mime);
	static void
	s_offer_source_actions(void *data, struct wl_data_offer *o, uint32_t actions);
	static void
	s_offer_action(void *data, struct wl_data_offer *o, uint32_t action);

	struct wl_display *m_display = nullptr;
	struct wl_data_device_manager *m_manager = nullptr;
	uint32_t m_manager_version = 0;
	struct wl_data_device *m_device = nullptr;
	struct wl_surface *m_content = nullptr;

	std::vector<Offer> m_offers;              //!< offers announced and not yet destroyed
	struct wl_data_offer *m_current = nullptr; //!< the drag over us now (accepted or not)
	bool m_current_ok = false;                 //!< ...it is on the content and offers text/uri-list
	double m_x = 0.0, m_y = 0.0;               //!< last drag position, surface-local logical

	//! The transfer of a dropped offer.
	struct wl_data_offer *m_receiving = nullptr;
	double m_drop_x = 0.0, m_drop_y = 0.0;
	dxr_input::FdDrain m_drain;
};

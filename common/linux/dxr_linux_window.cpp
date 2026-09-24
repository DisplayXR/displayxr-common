// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  THE desktop-Linux DisplayXR app window (X11 or native Wayland).
 *
 * See dxr_linux_window.h for the contract. The X11 leg started as the code
 * that used to live (twice, verbatim) in cube_handle_vk_linux/main.cpp and
 * cube_zones_vk_linux/main.cpp; the ICCCM size hints and the post-map
 * XMoveWindow survive from it, now followed by the EWMH fullscreen-on-monitor
 * placement a panel-sized window needs under mutter (#729).
 */

#include "dxr_linux_window.h"
#include "dxr_x11_chrome.h" // X11 glue for the header bar (displayxr::csd)

#include <X11/Xatom.h>   // XA_CARDINAL for _NET_WM_PID
#include <X11/XKBlib.h>  // XkbSetDetectableAutoRepeat — no fake KeyRelease on auto-repeat
#include <X11/keysym.h>  // XK_* for the X11 key mapping

#ifdef DXR_LW_HAVE_XSHAPE
#include <X11/extensions/shape.h> // ShapeInput — click-through input region
#endif

#ifdef DXR_APP_HAVE_XRANDR
#include <X11/extensions/Xrandr.h> // XRRGetMonitors — resolve the panel's monitor INDEX
#endif

#ifdef DXR_APP_HAVE_WAYLAND
#include "xdg-shell-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
// THE logical->device conversion, shared verbatim with the runtime so the
// app's output match and the runtime's window-rect conversion cannot drift
// apart (#1595/#1596). Header-only; see src/xrt/auxiliary/util/.
#include "util/u_wayland_geom.h"

#include "dxr_wl_chrome.h"            // DxrWlPointerEvent (content pointer forwarding)
#include <linux/input-event-codes.h> // raw evdev keycodes (the no-xkbcommon fallback table)
#include <poll.h>                    // non-blocking socket check in pump()
#include <sys/mman.h>                // mmap the compositor's keymap (xkbcommon builds)
#include <unistd.h>                  // close() the keymap fd
#ifdef DXR_LW_HAVE_XKBCOMMON
#include <xkbcommon/xkbcommon.h>
#endif
#endif

#include <dlfcn.h>  // libdbus-1 is loaded at run time for the probe — no build dependency
#include <unistd.h> // getpid() for _NET_WM_PID

#include <chrono> // bounded event-pump budget in the X11 placement handshake
#include <algorithm> // frame-stats percentile
#include <cmath>  // outward rounding of the Wayland input region
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread> // ...and the 5 ms breather between its polls

// Same shape as the test apps' own macros, so helper output is indistinguishable
// from app output in a run log. INFO flushes: stdout is block-buffered when a
// run is redirected to a file, and these apps are normally ended with a SIGTERM
// (`timeout 20 …`) that discards the buffer — which would take the placement
// line with it, exactly when it is wanted as evidence.
#define DXRW_INFO(fmt, ...)                                                                                            \
	do {                                                                                                           \
		fprintf(stdout, "[INFO]  " fmt "\n", ##__VA_ARGS__);                                                   \
		fflush(stdout);                                                                                        \
	} while (0)
#define DXRW_WARN(fmt, ...) fprintf(stderr, "[WARN]  " fmt "\n", ##__VA_ARGS__)
#define DXRW_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)


/*
 *
 * Backend selection.
 *
 */

const char *
DxrLinuxWindow::backend_name(DxrWindowBackend b)
{
	switch (b) {
	case DxrWindowBackend::X11: return "x11";
	case DxrWindowBackend::Wayland: return "wayland";
	default: return "auto";
	}
}

bool
DxrLinuxWindow::parse_backend(const char *text, DxrWindowBackend *out)
{
	if (text == nullptr || out == nullptr) {
		return false;
	}
	if (strcmp(text, "x11") == 0) {
		*out = DxrWindowBackend::X11;
		return true;
	}
	if (strcmp(text, "wayland") == 0 || strcmp(text, "wl") == 0) {
		*out = DxrWindowBackend::Wayland;
		return true;
	}
	if (strcmp(text, "auto") == 0) {
		*out = DxrWindowBackend::Auto;
		return true;
	}
	return false;
}

/*
 *
 * Capability probe + backend selection.
 *
 */

namespace {

/*!
 * THE policy point. `auto` prefers NATIVE Wayland when the probe says the
 * compositor is ready for it (DxrWindowProbe::wayland_ready():
 * wp_fractional_scale_v1 + wp_viewporter advertised AND the window-geometry
 * GNOME Shell extension owning org.displayxr.WindowGeometry on the session
 * bus), and X11 otherwise — which is what still covers Ubuntu 22.04 (GNOME 42
 * has no wp_fractional_scale_v1) and any session without the extension.
 * --platform=x11|wayland overrides it either way; the verdict is logged on
 * every auto run.
 *
 * WHY native first — CORRECTNESS, not GPU cost. Native Wayland has no
 * XWayland copy or resample, no global-scale quantisation of window placement
 * (XWayland runs the whole X screen at one integer scale, so a window cannot
 * reach every panel pixel), and an exact 1:1 buffer-to-panel mapping through
 * wp_viewporter + wp_fractional_scale_v1. The mid-drag weave shimmer that
 * used to argue for X11 is fixed by the compositor's drag lattice (runtime
 * #1609/#1686; dxr_wl_placement is its client here).
 *
 * Do NOT "optimise" this on GPU cost. A field report measured native Wayland
 * at 35% GPU against 60% through XWayland on an integrated GPU — UNCONFIRMED,
 * and most likely down to that run's smaller window and missing title bar.
 * A controlled A/B on an integrated GPU driving a 3840x2160 panel at 200%
 * (same app, 10 s GPU-busy each) found no backend advantage: Wayland 63.9% /
 * 63.7% (transparency-capable), X11 60.1% / 53.8% — the app's own rendering
 * dominates, and even that run was not like-for-like (a 3200x1800 Wayland
 * buffer against a 1920x1080 X11 one, which the size rule below now fixes).
 * Use --frame-stats to compare like with like.
 */
constexpr bool kAutoPrefersReadyWayland = true;

//! Session-bus probe through a run-time-loaded libdbus-1 (so the helper has
//! no build dependency on it). Answers "does org.displayxr.WindowGeometry
//! have an owner", i.e. is the window-geometry GNOME Shell extension live.
void
probe_session_bus(bool *bus_ok, bool *geometry_service)
{
	*bus_ok = false;
	*geometry_service = false;
	void *lib = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
	if (lib == nullptr) {
		return;
	}
	// Only the handful of entry points used here, with the ABI's own
	// signatures. DBusError is never needed: every call accepts a NULL error.
	typedef void *(*PfnBusGetPrivate)(int type, void *error);
	typedef void (*PfnSetExitOnDisconnect)(void *conn, unsigned int exit);
	typedef unsigned int (*PfnNameHasOwner)(void *conn, const char *name, void *error);
	typedef void (*PfnConnClose)(void *conn);
	typedef void (*PfnConnUnref)(void *conn);
	auto get_private = reinterpret_cast<PfnBusGetPrivate>(dlsym(lib, "dbus_bus_get_private"));
	auto set_exit = reinterpret_cast<PfnSetExitOnDisconnect>(dlsym(lib, "dbus_connection_set_exit_on_disconnect"));
	auto has_owner = reinterpret_cast<PfnNameHasOwner>(dlsym(lib, "dbus_bus_name_has_owner"));
	auto conn_close = reinterpret_cast<PfnConnClose>(dlsym(lib, "dbus_connection_close"));
	auto conn_unref = reinterpret_cast<PfnConnUnref>(dlsym(lib, "dbus_connection_unref"));
	if (get_private != nullptr && set_exit != nullptr && has_owner != nullptr && conn_close != nullptr &&
	    conn_unref != nullptr) {
		void *conn = get_private(0 /* DBUS_BUS_SESSION */, nullptr);
		if (conn != nullptr) {
			set_exit(conn, 0); // never let a bus hiccup exit() the app
			*bus_ok = true;
			*geometry_service = has_owner(conn, "org.displayxr.WindowGeometry", nullptr) != 0;
			conn_close(conn);
			conn_unref(conn);
		}
	}
	// Deliberately NOT dlclose'd: libdbus registers atexit-style state and
	// the runtime / display processor in this process may load it too.
}

#ifdef DXR_APP_HAVE_WAYLAND
struct WlProbeGlobals
{
	bool fractional_scale = false;
	bool viewporter = false;
};

void
wl_probe_global(void *data, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version)
{
	(void)r;
	(void)name;
	(void)version;
	auto *g = static_cast<WlProbeGlobals *>(data);
	if (strcmp(iface, "wp_fractional_scale_manager_v1") == 0) {
		g->fractional_scale = true;
	} else if (strcmp(iface, "wp_viewporter") == 0) {
		g->viewporter = true;
	}
}

void
wl_probe_global_remove(void *data, struct wl_registry *r, uint32_t name)
{
	(void)data;
	(void)r;
	(void)name;
}
#endif

} // namespace

std::string
DxrWindowProbe::describe() const
{
	std::string out = "X11 ";
	if (x11_connects) {
		out += std::string("connects (") + (x11_is_xwayland ? "XWayland" : "native X server") +
		       (x11_server.empty() ? "" : ", " + x11_server) + ")";
	} else {
		out += "does not connect";
	}
	out += "; Wayland ";
	if (!wayland_compiled) {
		out += "not compiled into this binary";
	} else if (!wayland_connects) {
		out += "does not connect";
	} else {
		out += std::string("connects (wp_fractional_scale_v1 ") + (wl_fractional_scale ? "yes" : "NO") +
		       ", wp_viewporter " + (wl_viewporter ? "yes" : "NO") + ")";
	}
	out += "; window-geometry extension on the session bus: ";
	out += !dbus_available ? "unknown (no session bus / libdbus-1)" : (geometry_service ? "yes" : "NO");
	return out;
}

DxrWindowProbe
DxrLinuxWindow::probe(bool wayland_details)
{
	DxrWindowProbe p;

	// X11 — XWayland counts. The connection is the capability.
	if (Display *dpy = XOpenDisplay(nullptr)) {
		p.x11_connects = true;
		int op = 0, ev = 0, err = 0;
		p.x11_is_xwayland = XQueryExtension(dpy, "XWAYLAND", &op, &ev, &err) != 0;
		char buf[160];
		snprintf(buf, sizeof(buf), "%s %d", ServerVendor(dpy), VendorRelease(dpy));
		p.x11_server = buf;
		XCloseDisplay(dpy);
	}

#ifdef DXR_APP_HAVE_WAYLAND
	p.wayland_compiled = true;
	if (struct wl_display *wd = wl_display_connect(nullptr)) {
		p.wayland_connects = true;
		if (wayland_details) {
			WlProbeGlobals g;
			struct wl_registry *reg = wl_display_get_registry(wd);
			static const struct wl_registry_listener kListener = {wl_probe_global, wl_probe_global_remove};
			wl_registry_add_listener(reg, &kListener, &g);
			wl_display_roundtrip(wd);
			p.wl_fractional_scale = g.fractional_scale;
			p.wl_viewporter = g.viewporter;
			wl_registry_destroy(reg);
		}
		wl_display_disconnect(wd);
	}
#else
	(void)wayland_details;
#endif

	if (wayland_details) {
		probe_session_bus(&p.dbus_available, &p.geometry_service);
	}
	return p;
}

//! --frame-stats request, set by the argument scanners; read by create().
static double s_frame_stats_request = 0.0;

//! `--frame-stats` / `--frame-stats=SECONDS`: true when @p a is that flag.
static bool
take_frame_stats_flag(const char *a)
{
	if (strcmp(a, "--frame-stats") == 0) {
		s_frame_stats_request = 5.0;
		return true;
	}
	if (strncmp(a, "--frame-stats=", 14) == 0) {
		const double v = atof(a + 14);
		s_frame_stats_request = v > 0.0 ? v : 5.0;
		return true;
	}
	return false;
}

bool
DxrLinuxWindow::parse_platform_args(int argc, char **argv, DxrWindowBackend *out, std::string *error)
{
	for (int i = 1; i < argc && argv != nullptr; i++) {
		const char *a = argv[i];
		if (a == nullptr || take_frame_stats_flag(a)) {
			continue;
		}
		const char *val = nullptr;
		const char *flag = nullptr;
		if (strncmp(a, "--platform=", 11) == 0) {
			flag = "--platform";
			val = a + 11;
		} else if (strncmp(a, "--backend=", 10) == 0) {
			flag = "--backend";
			val = a + 10;
		} else if (strcmp(a, "--platform") == 0) {
			flag = "--platform";
			if (i + 1 >= argc) {
				if (error != nullptr) {
					*error = "--platform needs a value: x11, wayland or auto";
				}
				return false;
			}
			val = argv[++i];
		} else {
			continue;
		}
		DxrWindowBackend b = DxrWindowBackend::Auto;
		if (!parse_backend(val, &b)) {
			if (error != nullptr) {
				*error = std::string(flag) + " must be one of x11|wayland|auto (got \"" + val + "\")";
			}
			return false;
		}
		if (out != nullptr) {
			*out = b;
		}
	}
	return true;
}

bool
DxrLinuxWindow::take_platform_args(std::vector<std::string> *args, DxrWindowBackend *out, std::string *error)
{
	if (args == nullptr) {
		return true;
	}
	std::vector<std::string> rest;
	rest.reserve(args->size());
	for (size_t i = 0; i < args->size(); i++) {
		const std::string &a = (*args)[i];
		if (take_frame_stats_flag(a.c_str())) {
			continue; // consumed
		}
		std::string val;
		std::string flag;
		if (a.rfind("--platform=", 0) == 0) {
			flag = "--platform";
			val = a.substr(11);
		} else if (a.rfind("--backend=", 0) == 0) {
			flag = "--backend";
			val = a.substr(10);
		} else if (a == "--platform") {
			if (i + 1 >= args->size()) {
				if (error != nullptr) {
					*error = "--platform needs a value: x11, wayland or auto";
				}
				return false;
			}
			flag = "--platform";
			val = (*args)[++i];
		} else {
			rest.push_back(a);
			continue;
		}
		DxrWindowBackend b = DxrWindowBackend::Auto;
		if (!parse_backend(val.c_str(), &b)) {
			if (error != nullptr) {
				*error = flag + " must be one of x11|wayland|auto (got \"" + val + "\")";
			}
			return false;
		}
		if (out != nullptr) {
			*out = b;
		}
	}
	args->swap(rest);
	return true;
}

DxrWindowBackend
DxrLinuxWindow::select(DxrWindowBackend requested, bool runtime_has_xlib, bool runtime_has_wayland, std::string *reason)
{
#ifndef DXR_APP_HAVE_WAYLAND
	// Built without libwayland: the Wayland leg is not compiled in at all.
	if (requested == DxrWindowBackend::Wayland) {
		if (reason != nullptr) {
			*reason = "this binary was built without libwayland-client (no Wayland backend compiled in)";
		}
		return DxrWindowBackend::Auto;
	}
	runtime_has_wayland = false;
#endif

	// 1. An explicit request always wins. The connection is still probed, but
	//    only so the reason can say up front that it is going to fail.
	if (requested == DxrWindowBackend::X11) {
		if (!runtime_has_xlib) {
			if (reason != nullptr) {
				*reason = "--platform=x11 but the runtime does not advertise " +
				          std::string(XR_DXR_XLIB_WINDOW_BINDING_EXTENSION_NAME);
			}
			return DxrWindowBackend::Auto;
		}
		const DxrWindowProbe p = probe(false);
		if (reason != nullptr) {
			*reason = p.x11_connects ? std::string("explicitly requested (") +
			                               (p.x11_is_xwayland ? "XWayland" : "native X server") + ")"
			                         : "explicitly requested (no X server answers — window creation will fail)";
		}
		return DxrWindowBackend::X11;
	}

	if (requested == DxrWindowBackend::Wayland) {
		if (!runtime_has_wayland) {
			if (reason != nullptr) {
				*reason = "--platform=wayland but the runtime does not advertise " +
				          std::string(XR_DXR_WAYLAND_SURFACE_BINDING_EXTENSION_NAME);
			}
			return DxrWindowBackend::Auto;
		}
		const DxrWindowProbe p = probe(false);
		if (reason != nullptr) {
			*reason = p.wayland_connects
			              ? "explicitly requested"
			              : "explicitly requested (no Wayland compositor answers — window creation will fail)";
		}
		return DxrWindowBackend::Wayland;
	}

	// 2-4. Auto: probe everything, log the Wayland-ready verdict, apply the
	//      one policy constant.
	const DxrWindowProbe p = probe(true);
	const bool ready = p.wayland_ready();
	DXRW_INFO("Window platform probe: %s", p.describe().c_str());
	DXRW_INFO("Window platform probe: Wayland-ready = %s (fractional-scale + viewporter + window-geometry "
	          "extension); policy: %s",
	          ready ? "YES" : "no",
	          kAutoPrefersReadyWayland ? "prefer native Wayland when ready, else X11"
	                                   : "prefer X11 (native Wayland only as the fallback)");

	if (kAutoPrefersReadyWayland && ready && runtime_has_wayland) {
		if (reason != nullptr) {
			*reason = "auto: the compositor is Wayland-ready and the policy prefers native Wayland";
		}
		return DxrWindowBackend::Wayland;
	}
	if (p.x11_connects && runtime_has_xlib) {
		if (reason != nullptr) {
			*reason = std::string("auto: an X server answers (") +
			          (p.x11_is_xwayland ? "XWayland" : "native X server") +
			          ") and the runtime advertises the xlib binding — X11 is the proven path" +
			          (ready ? " (native Wayland is ready too; the policy still prefers X11)" : "");
		}
		return DxrWindowBackend::X11;
	}
	if (p.wayland_connects && runtime_has_wayland) {
		if (reason != nullptr) {
			*reason = std::string("auto: ") +
			          (p.x11_connects ? "the runtime does not advertise the xlib binding"
			                          : "no X server answers") +
			          ", falling back to native Wayland";
		}
		return DxrWindowBackend::Wayland;
	}

	if (reason != nullptr) {
		*reason = "neither backend usable: X11 " + std::string(p.x11_connects ? "connects" : "does not connect") +
		          ", xlib_binding=" + (runtime_has_xlib ? "yes" : "no") + "; Wayland " +
		          (p.wayland_connects ? "connects" : (p.wayland_compiled ? "does not connect" : "not compiled in")) +
		          ", wayland_binding=" + (runtime_has_wayland ? "yes" : "no");
	}
	return DxrWindowBackend::Auto;
}


/*
 *
 * X11 leg.
 *
 */

namespace {

//! Rect of the RandR monitor a fullscreen request was targeted at.
struct X11MonitorRect
{
	int index = -1; //!< RandR monitor index, or -1 when unresolved
	int x = 0, y = 0;
	int width = 0, height = 0;
	std::string name = "?";
};

/*!
 * Resolve the RandR monitor INDEX that owns (@p left, @p top), for the
 * _NET_WM_FULLSCREEN_MONITORS request. Xlib mirror of the runtime's own
 * resolve_monitor_index() in comp_vk_native_window_xcb.c (#723).
 *
 * Prefers a monitor whose origin exactly matches the point; falls back to the
 * monitor CONTAINING it. Returns index -1 when RandR is unavailable (the
 * caller then falls back to plain _NET_WM_STATE_FULLSCREEN, which mutter
 * applies to the output the window currently occupies).
 */
X11MonitorRect
x11_resolve_monitor(Display *dpy, ::Window root, int32_t left, int32_t top)
{
	X11MonitorRect out;
#ifdef DXR_APP_HAVE_XRANDR
	int count = 0;
	XRRMonitorInfo *mons = XRRGetMonitors(dpy, root, True /* active only */, &count);
	if (mons == nullptr) {
		return out;
	}

	int exact = -1;
	int contains = -1;
	for (int i = 0; i < count; i++) {
		if (exact < 0 && mons[i].x == (int)left && mons[i].y == (int)top) {
			exact = i;
		}
		if (contains < 0 && left >= mons[i].x && left < mons[i].x + mons[i].width && top >= mons[i].y &&
		    top < mons[i].y + mons[i].height) {
			contains = i;
		}
	}

	const int chosen = exact >= 0 ? exact : contains;
	if (chosen >= 0) {
		out.index = chosen;
		out.x = mons[chosen].x;
		out.y = mons[chosen].y;
		out.width = mons[chosen].width;
		out.height = mons[chosen].height;
		if (mons[chosen].name != None) {
			char *nm = XGetAtomName(dpy, mons[chosen].name);
			if (nm != nullptr) {
				out.name = nm;
				XFree(nm);
			}
		}
	}
	XRRFreeMonitors(mons);
#else
	(void)dpy;
	(void)root;
	(void)left;
	(void)top;
#endif
	return out;
}

/*!
 * Drain the X event queue for up to @p budget_ms, returning early as soon as
 * @p done() is true. Never blocks indefinitely: XPending never waits, and the
 * deadline is monotonic.
 *
 * Called only during create(), before the app's own pump() exists, so the
 * events drained here are startup noise (Map/Configure/Reparent) that nothing
 * is listening for yet.
 */
void
x11_pump_for(Display *dpy, int budget_ms, const std::function<bool()> &done)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
	for (;;) {
		XSync(dpy, False); // push our requests, take in whatever the WM replied
		while (XPending(dpy) > 0) {
			XEvent ev;
			XNextEvent(dpy, &ev);
		}
		if (done && done()) {
			return;
		}
		if (std::chrono::steady_clock::now() >= deadline) {
			return;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
}

//! Root-relative origin of @p win (what the WM actually did with it), as
//! opposed to XGetWindowAttributes' x/y, which are parent-relative and so read
//! as an offset INSIDE the frame once a WM reparents the window.
void
x11_root_origin(Display *dpy, ::Window win, int *out_x, int *out_y)
{
	::Window child = 0;
	int rx = 0;
	int ry = 0;
	if (XTranslateCoordinates(dpy, win, DefaultRootWindow(dpy), 0, 0, &rx, &ry, &child) == 0) {
		rx = 0;
		ry = 0;
	}
	*out_x = rx;
	*out_y = ry;
}

//! Post an EWMH client message to the root window (the WM listens for these on
//! SubstructureRedirect|Notify).
void
x11_send_root_message(Display *dpy, ::Window win, Atom type, long d0, long d1, long d2, long d3, long d4)
{
	XEvent ev = {};
	ev.xclient.type = ClientMessage;
	ev.xclient.send_event = True;
	ev.xclient.display = dpy;
	ev.xclient.window = win;
	ev.xclient.message_type = type;
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = d0;
	ev.xclient.data.l[1] = d1;
	ev.xclient.data.l[2] = d2;
	ev.xclient.data.l[3] = d3;
	ev.xclient.data.l[4] = d4;
	XSendEvent(dpy, DefaultRootWindow(dpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
}

/*!
 * Ask for an undecorated toplevel through the Motif hints.
 *
 * Belt-and-braces next to _NET_WM_STATE_FULLSCREEN: a fullscreen window is
 * undecorated by EWMH rule anyway, but a WM that ignores EWMH (or that decides
 * not to honour the fullscreen request) would otherwise reparent us into a
 * title bar and steal 74 px off the top of the weave — exactly the #729
 * symptom. Both mutter and KWin honour _MOTIF_WM_HINTS.
 */
void
x11_set_undecorated(Display *dpy, ::Window win)
{
	Atom motif = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
	if (motif == None) {
		return;
	}
	// flags, functions, decorations, input_mode, status — flags=2 is
	// MWM_HINTS_DECORATIONS, decorations=0 is "none".
	unsigned long hints[5] = {2, 0, 0, 0, 0};
	XChangeProperty(dpy, win, motif, motif, 32, PropModeReplace, (const unsigned char *)hints, 5);
}

} // namespace

bool
DxrLinuxWindow::create_x11(const DxrLinuxWindowDesc &desc)
{
	int32_t screenLeft = desc.panel_left;
	int32_t screenTop = desc.panel_top;

	// A window asking for exactly the panel's size IS the fullscreen demo
	// mode, and must be genuinely fullscreen on the panel: exact 1:1, no
	// decoration, no offset. Anything smaller is a deliberate windowed run
	// (DXR_CUBE_WINDOW) and keeps the plain create-at-position behaviour.
	// DXR_X11_NO_FULLSCREEN=1 forces the old path for A/B testing.
	const char *no_fs = getenv("DXR_X11_NO_FULLSCREEN");
	const bool fs_opt_out = no_fs != nullptr && no_fs[0] != '\0' && strcmp(no_fs, "0") != 0;
	const bool panel_known = desc.panel_width > 0 && desc.panel_height > 0;
	const bool want_fullscreen =
	    panel_known && !fs_opt_out && desc.width == desc.panel_width && desc.height == desc.panel_height;

	// #1588: a WINDOWED toplevel is undecorated + client-dragged as well, so
	// every move can be routed through the weave's lattice snap. Opt out for
	// the old decorated, WM-dragged behaviour.
	const char *wm_dec = getenv("DXR_X11_WM_DECORATIONS");
	m_x_wm_drag = !want_fullscreen && wm_dec != nullptr && wm_dec[0] != '\0' && strcmp(wm_dec, "0") != 0;
	const bool client_drag = !want_fullscreen && !m_x_wm_drag;
	m_x_client_drag = client_drag;
	m_x_fullscreen = want_fullscreen;

	// An app-chosen windowed position (e.g. centred on the panel). The
	// fullscreen shape keeps the panel origin: that IS the placement.
	if (desc.has_position && !want_fullscreen) {
		screenLeft = desc.x;
		screenTop = desc.y;
	}

	// Bounded retry: an X server with no other clients (Xvfb, a bare kiosk
	// Xorg) RESETS when its last client disconnects and refuses connections
	// while it regenerates — which is exactly what the capability probe's
	// open/close just before this can trigger. A desktop session always has
	// other clients and connects first time. Measured on CI: without this,
	// a create right after a close failed intermittently.
	for (int attempt = 0; attempt < 20 && m_x_display == nullptr; attempt++) {
		m_x_display = XOpenDisplay(nullptr);
		if (m_x_display == nullptr) {
			std::this_thread::sleep_for(std::chrono::milliseconds(25));
		} else if (attempt > 0) {
			DXRW_INFO("XOpenDisplay succeeded on attempt %d (the X server was resetting)", attempt + 1);
		}
	}
	if (m_x_display == nullptr) {
		DXRW_ERROR("XOpenDisplay failed — no X server answers");
		return false;
	}

	// INV-1.3: open on the 3D panel (#715). (screenLeft, screenTop) is the
	// panel top-left in virtual-desktop pixels (top-down, origin = primary
	// top-left, XrDisplayDesktopPositionDXR); (0,0) = primary/unknown is a
	// safe create position either way.
	int screen = DefaultScreen(m_x_display);
	::Window root = RootWindow(m_x_display, screen);

	// Transparent-background capability. The visual is fixed for the
	// window's whole life, so it is decided HERE, before the session exists:
	// a depth-32 TrueColor visual is what lets the runtime's swapchain
	// advertise a non-opaque compositeAlpha and a compositing WM blend the
	// window over the desktop. No such visual -> opaque, and is_transparent()
	// says so (the app keeps the handle path, it just loses transparency).
	XVisualInfo vinfo = {};
	bool argb = false;
	if (desc.transparent) {
		argb = XMatchVisualInfo(m_x_display, screen, 32, TrueColor, &vinfo) != 0;
		if (!argb) {
			DXRW_WARN("No 32-bit ARGB visual on this X screen — the transparent background is unavailable; "
			          "continuing opaque on the root visual");
		}
	}
	m_transparent = argb;
	// Launched drawing transparent: the header bar starts hidden.
	m_transparent_bg = argb && desc.transparent_background;

	// Header bar (desc.x11_header_bar): built whenever the WM does not draw
	// the frame, SHOWN only while windowed (x11_bar_visible()). Its height
	// follows the desktop scale, so it is known before anything is created.
	// (desc.width x desc.height) at (screenLeft, screenTop) is the CONTENT
	// rect — what the runtime sees — and the top-level grows UP by the bar,
	// so an app's "WxH+X+Y" names the 3D area exactly, which is the rect the
	// weave phase depends on.
	m_x_bar_enabled = desc.x11_header_bar;
	uint32_t bar_h = 0;
	if (m_x_bar_enabled) {
		m_x_bar.configure(dxr_x11_chrome::DesktopScale(m_x_display));
		// Translucent + rounded only where the alpha is real (ARGB visual,
		// composited); an opaque visual gets an opaque, square bar.
		m_x_bar.setSurfaceHasAlpha(argb);
		// X11 resize stays the WM's (Super+middle-drag); the bar is a drag
		// handle and two buttons.
		m_x_bar.setResizable(false);
		m_x_bar.setTitle(desc.title != nullptr ? desc.title : "");
		bar_h = (want_fullscreen || m_x_wm_drag || m_transparent_bg) ? 0 : m_x_bar.height();
		DXRW_INFO("X11 header bar: %u px (scale %.2f), %s, title font %s", m_x_bar.height(), m_x_bar.scale(),
		          argb ? "translucent with rounded corners (ARGB visual)" : "opaque, square (no ARGB visual)",
		          m_x_bar.fontPath().empty() ? "NONE (set DXR_CSD_FONT)" : m_x_bar.fontPath().c_str());
	}
	m_x_content_w = desc.width;
	m_x_content_h = desc.height;
	m_x_top_w = desc.width;
	m_x_top_h = desc.height + bar_h;
	const int top_x = screenLeft;
	const int top_y = screenTop - (int)bar_h;

	if (!argb && !m_x_bar_enabled) {
		// The plain window — byte-for-byte the pre-common behaviour of the
		// runtime's cube apps.
		m_x_window = XCreateSimpleWindow(m_x_display, root, screenLeft, screenTop, desc.width, desc.height, 0,
		                                 BlackPixel(m_x_display, screen), BlackPixel(m_x_display, screen));
		if (m_x_window == 0) {
			DXRW_ERROR("XCreateSimpleWindow failed");
			return false;
		}
		m_x_visual = DefaultVisual(m_x_display, screen);
	} else {
		// A window whose depth differs from its parent's must carry its own
		// colormap AND an explicit border pixel, or X raises BadMatch.
		XSetWindowAttributes attrs = {};
		attrs.background_pixel = argb ? 0UL : BlackPixel(m_x_display, screen);
		attrs.border_pixel = 0;
		unsigned long mask = CWBackPixel | CWBorderPixel;
		int depth = CopyFromParent;
		Visual *visual = CopyFromParent;
		if (argb) {
			m_x_colormap = XCreateColormap(m_x_display, root, vinfo.visual, AllocNone);
			attrs.colormap = m_x_colormap;
			mask |= CWColormap;
			depth = 32;
			visual = vinfo.visual;
		}
		m_x_window = XCreateWindow(m_x_display, root, top_x, top_y, m_x_top_w, m_x_top_h, 0, depth, InputOutput,
		                           visual, mask, &attrs);
		if (m_x_window == 0) {
			DXRW_ERROR("XCreateWindow failed");
			return false;
		}
		m_x_visual = argb ? vinfo.visual : DefaultVisual(m_x_display, screen);

		if (m_x_bar_enabled) {
			// The CONTENT child: same depth / visual / colormap as the
			// top-level (so XWayland composites it straight into the
			// top-level's buffer with its alpha intact), background None so
			// the server never paints over what Vulkan presents, and NO event
			// mask — pointer and key events propagate to the top-level, which
			// handles everything in top-level coordinates.
			XSetWindowAttributes ca = {};
			ca.background_pixmap = None;
			ca.border_pixel = 0;
			unsigned long cmask = CWBackPixmap | CWBorderPixel;
			if (argb) {
				ca.colormap = m_x_colormap;
				cmask |= CWColormap;
			}
			m_x_content = XCreateWindow(m_x_display, m_x_window, 0, (int)bar_h, desc.width, desc.height, 0,
			                            depth, InputOutput, visual, cmask, &ca);
			if (m_x_content == 0) {
				DXRW_ERROR("XCreateWindow (content child) failed");
				return false;
			}
			XMapWindow(m_x_display, m_x_content); // shown with the parent below
			m_x_content_off_applied = (int)bar_h;
			// The top-level's own background shows only under the bar, which
			// is painted; None avoids an ARGB clear flashing there on Expose.
			XSetWindowBackgroundPixmap(m_x_display, m_x_window, None);
		}
	}

	// _NET_WM_PID: what lets the compositor attribute this window to this
	// process. The window-geometry service filters by PID, and the display
	// processor's capture exclusion (CaptureExclusion1.Exclude(0) — every
	// window of the calling PROCESS) keys on it; on native Wayland the
	// compositor knows the PID from the socket, on X11 it is this property.
	{
		Atom net_wm_pid = XInternAtom(m_x_display, "_NET_WM_PID", False);
		const unsigned long pid = (unsigned long)getpid();
		XChangeProperty(m_x_display, m_x_window, net_wm_pid, XA_CARDINAL, 32, PropModeReplace,
		                (const unsigned char *)&pid, 1);
	}

	// WM_NORMAL_HINTS with USPosition|PPosition, so the window manager treats
	// the create-time position as intentional instead of auto-placing the
	// window (ICCCM §4.1.2.3; GNOME/Mutter auto-places without this). Mirrors
	// the runtime's own hosted-window placement (comp_vk_native_window_xcb.c).
	{
		XSizeHints hints = {};
		hints.flags = USPosition | PPosition;
		hints.x = top_x;
		hints.y = top_y;
		XSetWMNormalHints(m_x_display, m_x_window, &hints);
	}

	XStoreName(m_x_display, m_x_window, desc.title);
	// Button + motion events are what the client-owned drag runs on (#1588),
	// and — with the event API — what the app's own pointer input runs on.
	// Enter/Leave/Focus/Expose drive the header bar's hover, backdrop and
	// repaint. Selecting them unconditionally keeps one input mask for every
	// window shape.
	XSelectInput(m_x_display, m_x_window,
	             StructureNotifyMask | KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
	                 PointerMotionMask | Button1MotionMask | Button2MotionMask | Button3MotionMask |
	                 EnterWindowMask | LeaveWindowMask | FocusChangeMask | ExposureMask);

	// Held keys must not stutter: without this X delivers auto-repeat as
	// Release/Press pairs, which reads as the key being let go.
	{
		Bool supported = False;
		XkbSetDetectableAutoRepeat(m_x_display, True, &supported);
		m_x_detectable_repeat = supported == True;
	}

	// Keep-above requested at create: set as a property BEFORE the map, which
	// is when the WM reads the initial _NET_WM_STATE.
	if (desc.keep_above && !want_fullscreen) {
		Atom net_wm_state = XInternAtom(m_x_display, "_NET_WM_STATE", False);
		Atom above = XInternAtom(m_x_display, "_NET_WM_STATE_ABOVE", False);
		XChangeProperty(m_x_display, m_x_window, net_wm_state, XA_ATOM, 32, PropModeReplace,
		                (const unsigned char *)&above, 1);
		m_x_keep_above = true;
	}

	// Clean close on the window manager's close button.
	m_x_wm_delete = XInternAtom(m_x_display, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(m_x_display, m_x_window, &m_x_wm_delete, 1);

	// Decorations off BEFORE the map, so a frame is never created in the first
	// place. Mutter reparenting us into a title bar is what clamped the #729
	// window to 3840x2086 at (3456, 74) — and for a WINDOWED run (#1588) the
	// frame is worse than an offset: it hands the drag to the WM, where the
	// interlace phase cannot be snapped. Undecorated is therefore the default
	// for both, and DXR_X11_WM_DECORATIONS=1 is the escape hatch.
	if (want_fullscreen || client_drag) {
		x11_set_undecorated(m_x_display, m_x_window);
	}

	XMapWindow(m_x_display, m_x_window);
	XFlush(m_x_display);

	// Re-assert the position after mapping — many WMs (Mutter included)
	// ignore the create-time x/y of a freshly mapped toplevel, but honor a
	// post-map ConfigureRequest (this is what `xdotool windowmove` sends).
	XMoveWindow(m_x_display, m_x_window, top_x, top_y);
	XFlush(m_x_display);

	X11MonitorRect mon;
	if (want_fullscreen) {
		::Window root = DefaultRootWindow(m_x_display);
		mon = x11_resolve_monitor(m_x_display, root, screenLeft, screenTop);

		// Let the move land before asking for fullscreen: mutter fullscreens
		// onto whichever output the window CURRENTLY occupies, and a position
		// request issued before the window settles is simply discarded. Wait
		// for the window's root origin to reach the target monitor, with a
		// hard 400 ms ceiling so a WM that never moves us cannot hang startup.
		const int mon_x = mon.index >= 0 ? mon.x : (int)screenLeft;
		const int mon_y = mon.index >= 0 ? mon.y : (int)screenTop;
		const int mon_w = mon.index >= 0 ? mon.width : 1;
		const int mon_h = mon.index >= 0 ? mon.height : 1;
		Display *dpy = m_x_display;
		::Window win = m_x_window;
		x11_pump_for(dpy, 400, [dpy, win, mon_x, mon_y, mon_w, mon_h]() {
			int rx = 0;
			int ry = 0;
			x11_root_origin(dpy, win, &rx, &ry);
			return rx >= mon_x && rx < mon_x + mon_w && ry >= mon_y && ry < mon_y + mon_h;
		});

		// EWMH fullscreen. _NET_WM_STATE add first (source 1 = application),
		// then pin it to the panel's RandR monitor: single-monitor fullscreen
		// means all four edges are the same index. Without RandR we still send
		// the fullscreen request — mutter applies it to the output the window
		// now occupies, which the pump above just made the right one.
		Atom net_wm_state = XInternAtom(m_x_display, "_NET_WM_STATE", False);
		Atom net_wm_state_fullscreen = XInternAtom(m_x_display, "_NET_WM_STATE_FULLSCREEN", False);
		if (net_wm_state != None && net_wm_state_fullscreen != None) {
			x11_send_root_message(m_x_display, m_x_window, net_wm_state, 1 /* _NET_WM_STATE_ADD */,
			                      (long)net_wm_state_fullscreen, 0, 1 /* source: application */, 0);
		}
		if (mon.index >= 0) {
			Atom net_fs_monitors = XInternAtom(m_x_display, "_NET_WM_FULLSCREEN_MONITORS", False);
			if (net_fs_monitors != None) {
				x11_send_root_message(m_x_display, m_x_window, net_fs_monitors, mon.index /* top */,
				                      mon.index /* bottom */, mon.index /* left */,
				                      mon.index /* right */, 1 /* source: application */);
			}
		}
		XFlush(m_x_display);

		// Second bounded wait: let the fullscreen configure arrive, so the log
		// line below (and the runtime's first swapchain sizing) sees the truth.
		const uint32_t want_w = desc.width;
		const uint32_t want_h = desc.height; // a fullscreen top-level carries no bar
		x11_pump_for(dpy, 400, [dpy, win, want_w, want_h]() {
			XWindowAttributes wa = {};
			return XGetWindowAttributes(dpy, win, &wa) != 0 && (uint32_t)wa.width == want_w &&
			       (uint32_t)wa.height == want_h;
		});
	} else {
		// Windowed: no fullscreen handshake, but still give the WM a moment to
		// reparent and place us, so the line below reports where the window
		// really ended up rather than where it was a millisecond after the
		// move request (under mutter those differ by the frame's title bar).
		x11_pump_for(m_x_display, 250, {});
	}

	// One line, after the handshake: what was asked for and what the WM
	// actually did. XTranslateCoordinates is the honest origin (XGetWindow-
	// Attributes' x/y are parent-relative once a WM reparents us), so this
	// proves placement without reaching for xwininfo.
	{
		XWindowAttributes wa = {};
		int rx = 0;
		int ry = 0;
		XGetWindowAttributes(m_x_display, m_x_window, &wa);
		x11_root_origin(m_x_display, x11_bound_window(), &rx, &ry);
		m_x_top_w = wa.width > 0 ? (uint32_t)wa.width : m_x_top_w;
		m_x_top_h = wa.height > 0 ? (uint32_t)wa.height : m_x_top_h;
		if (want_fullscreen) {
			char where[96];
			if (mon.index >= 0) {
				snprintf(where, sizeof(where), "fullscreen on RandR monitor #%d (%s)", mon.index,
				         mon.name.c_str());
			} else {
				snprintf(where, sizeof(where), "fullscreen on the current output (no RandR monitor at "
				                               "the panel position)");
			}
			DXRW_INFO("Created app-owned X11 window 0x%lx: requested %ux%u at (%d, %d) %s; actual %dx%d "
			          "at (%d, %d)",
			          m_x_window, desc.width, desc.height, screenLeft, screenTop, where, wa.width,
			          wa.height, rx, ry);
		} else {
                  DXRW_INFO("Created app-owned X11 window 0x%lx: requested "
                            "%ux%u at (%d, %d) windowed%s%s; "
                            "actual %dx%d at (%d, %d)",
                            m_x_window, desc.width, desc.height, screenLeft,
                            screenTop,
                            fs_opt_out ? " (DXR_X11_NO_FULLSCREEN)" : "",
                            client_drag ? ", undecorated + client-owned drag "
                                          "(snap offered by the display "
                                          "processor; each landing is "
                                          "verified, see 'drag: placement')"
                                        : " (DXR_X11_WM_DECORATIONS)",
                            wa.width, wa.height, rx, ry);
                }
		m_x_drag_at_x = rx;
		m_x_drag_at_y = ry;
		if (m_x_content != 0) {
			DXRW_INFO("X11: bound window is the content child 0x%lx (%ux%u) under a %u px header bar%s",
			          m_x_content, desc.width, desc.height, m_x_bar.height(),
			          want_fullscreen ? " (hidden while fullscreen)"
			                          : (m_transparent_bg ? " (hidden while the background is transparent)" : ""));
		}
		if (m_transparent) {
			DXRW_INFO("X11: 32-bit ARGB visual — transparent-background capable (a compositing WM must be "
			          "running for the desktop to show through)");
		}
	}
	x11_layout_content();

	// DXR_X11_TEST_DRAG=dx,dy,steps — TEST HOOK, off by default. Only meaningful
	// where a drag is possible at all (windowed + client-owned).
	if (client_drag) {
		if (const char *tenv = getenv("DXR_X11_TEST_DRAG")) {
			int dx = 0, dy = 0, steps = 0;
			if (sscanf(tenv, "%d,%d,%d", &dx, &dy, &steps) == 3 && steps > 0) {
				m_x_test_drag_armed = true;
				m_x_test_drag_dx = dx;
				m_x_test_drag_dy = dy;
				m_x_test_drag_steps = steps;
				DXRW_WARN("DXR_X11_TEST_DRAG=%d,%d,%d — TEST HOOK armed; the window will walk "
				          "that offset in %d snapped steps after a warm-up",
				          dx, dy, steps, steps);
			} else {
				DXRW_WARN("DXR_X11_TEST_DRAG=\"%s\" is not dx,dy,steps — ignored", tenv);
			}
		}
	}
	return true;
}

/*
 *
 * X11 client-owned drag (#1588).
 *
 */

void
DxrLinuxWindow::snap_origin(int origin_x, int origin_y, int target_x, int target_y, int *out_x, int *out_y)
{
	int32_t sx = (int32_t)target_x;
	int32_t sy = (int32_t)target_y;
	bool snapped = false;
	if (m_snap_fn != nullptr) {
		snapped = m_snap_fn(m_snap_userdata, (int32_t)origin_x, (int32_t)origin_y, (int32_t)target_x,
		                    (int32_t)target_y, &sx, &sy);
	}
	if (!snapped) {
		sx = (int32_t)target_x;
		sy = (int32_t)target_y;
	}
	if (!m_snap_reported) {
		m_snap_reported = true;
                DXRW_INFO("drag: snap provider %s — %s",
                          m_snap_fn != nullptr ? "installed"
                                               : "ABSENT (identity)",
                          snapped ? "the display processor is OFFERING snapped "
                                    "origins — whether the window "
                                    "actually lands on them is checked per "
                                    "move ('drag: placement')"
                                  : "identity (no DP lattice snap on this "
                                    "runtime, or the runtime refused "
                                    "the snap — see its log); the drag "
                                    "mechanics are unaffected");
        }
	*out_x = (int)sx;
	*out_y = (int)sy;
}

void
DxrLinuxWindow::x11_move_snapped(int target_x, int target_y)
{
	if (m_x_display == nullptr || m_x_window == 0) {
		return;
	}
        x11_check_landing();

        int sx = target_x;
	int sy = target_y;
	snap_origin(m_x_drag_origin_x, m_x_drag_origin_y, target_x, target_y, &sx, &sy);

	if (sx != target_x || sy != target_y) {
		m_x_drag_snapped++;
		// On-change only: with an identity snap this never fires, which is
		// the point — a per-motion log line would be per-event spam.
		DXRW_INFO("drag: raw (%d, %d) -> snapped (%d, %d)", target_x, target_y, sx, sy);
	}
	if (sx == m_x_drag_at_x && sy == m_x_drag_at_y) {
		return; // the lattice swallowed this step; do not churn the WM
	}
	// Drag coordinates are the BOUND window's (the content child under a
	// header bar): that is the rect the weave phase — and so the snap — is
	// about. The WM moves the top-level, so subtract the constant
	// content-in-top-level offset captured at the grab.
	XMoveWindow(m_x_display, m_x_window, sx - m_x_drag_off_x, sy - m_x_drag_off_y);
	XFlush(m_x_display);
	m_x_drag_at_x = sx;
	m_x_drag_at_y = sy;
	m_x_drag_moves++;
        m_x_probe_pending = true;
        m_x_probe_want_x = sx;
        m_x_probe_want_y = sy;
}

void DxrLinuxWindow::x11_check_landing() {
  if (!m_x_probe_pending || m_x_display == nullptr || m_x_window == 0) {
    return;
  }
  m_x_probe_pending = false;
  int got_x = 0, got_y = 0;
  x11_root_origin(m_x_display, x11_bound_window(), &got_x, &got_y);
  u_x11_placement_probe_note(&m_x_probe, m_x_probe_want_x, m_x_probe_want_y,
                             got_x, got_y);

  if (!m_x_probe.reported && u_x11_placement_probe_is_quantized(&m_x_probe)) {
    m_x_probe.reported = true;
    // One line, once per process: the claim a snapped drag makes is
    // "the window lands where the lens wants it". It does not, so say so,
    // and name the cause that has actually produced this.
    DXRW_WARN("drag: placement NOT honoured — %u of %u moves landed somewhere "
              "other than the "
              "requested origin (worst %u px); landed positions fall on a %u "
              "px lattice. The "
              "window cannot reach every pixel, so the 3D will stutter while "
              "dragging. Most "
              "likely cause: XWayland is running the X screen at global scale "
              "%u because some "
              "output (often NOT the 3D panel) is scaled above 100%%. Fix: "
              "every output at "
              "100%%, or set DXR_X11_PLACEMENT_QUANTUM=%u so the runtime snaps "
              "on the reachable "
              "lattice. `displayxr-cli info` shows the per-output evidence.",
              m_x_probe.diverged, m_x_probe.moves, m_x_probe.worst_delta,
              m_x_probe.inferred_quantum, m_x_probe.inferred_quantum,
              m_x_probe.inferred_quantum);
  }
}

void
DxrLinuxWindow::x11_drive_test_drag()
{
	// Warm-up: let the session come up and the first frames present before
	// the window starts moving, so the runtime's origin trace is readable.
	const uint64_t kWarmupFrames = 60;
	if (!m_x_test_drag_armed || m_x_test_drag_done || m_x_pump_count < kWarmupFrames) {
		return;
	}

	if (m_x_test_drag_step == 0) {
		// Same bookkeeping a ButtonPress does: latch the drag origin (of the
		// bound window) and the bound-in-top-level offset.
		int top_x = 0, top_y = 0;
		x11_root_origin(m_x_display, m_x_window, &top_x, &top_y);
		x11_root_origin(m_x_display, x11_bound_window(), &m_x_drag_origin_x, &m_x_drag_origin_y);
		m_x_drag_off_x = m_x_drag_origin_x - top_x;
		m_x_drag_off_y = m_x_drag_origin_y - top_y;
		m_x_drag_at_x = m_x_drag_origin_x;
		m_x_drag_at_y = m_x_drag_origin_y;
		m_x_drag_moves = 0;
		m_x_drag_snapped = 0;
		DXRW_INFO("drag: start (TEST HOOK) — grab origin (%d, %d), walking %+d,%+d in %d steps",
		          m_x_drag_origin_x, m_x_drag_origin_y, m_x_test_drag_dx, m_x_test_drag_dy,
		          m_x_test_drag_steps);
	}

	m_x_test_drag_step++;
	// Integer-exact endpoint: step i of N lands on origin + d*i/N, so the last
	// step is origin + d with no rounding residue.
	const int i = m_x_test_drag_step;
	const int n = m_x_test_drag_steps;
	const int tx = m_x_drag_origin_x + (int)((int64_t)m_x_test_drag_dx * i / n);
	const int ty = m_x_drag_origin_y + (int)((int64_t)m_x_test_drag_dy * i / n);
	x11_move_snapped(tx, ty);

	if (i >= n) {
		m_x_test_drag_done = true;
                DXRW_INFO("drag: end (TEST HOOK) — %llu move(s), %llu snapped "
                          "away from the raw target, "
                          "origin (%d, %d) -> (%d, %d); placement so far: %u "
                          "of %u verified moves landed exactly",
                          (unsigned long long)m_x_drag_moves,
                          (unsigned long long)m_x_drag_snapped,
                          m_x_drag_origin_x, m_x_drag_origin_y, m_x_drag_at_x,
                          m_x_drag_at_y, m_x_probe.moves - m_x_probe.diverged,
                          m_x_probe.moves);
        }
}

bool
DxrLinuxWindow::x11_bar_visible() const
{
	// Windowed only: a fullscreen app has no title bar anywhere on GNOME, and
	// the bar must never eat pixels out of a panel-sized weave.
	return m_x_bar_enabled && m_x_content != 0 && !m_x_fullscreen && !m_x_wm_drag && !m_transparent_bg;
}

int
DxrLinuxWindow::x11_content_offset_y() const
{
	return x11_bar_visible() ? (int)m_x_bar.height() : 0;
}

void
DxrLinuxWindow::x11_layout_content()
{
	if (m_x_display == nullptr || m_x_window == 0 || m_x_top_w == 0 || m_x_top_h == 0) {
		return;
	}
	if (m_x_content == 0) {
		// No child: the top-level IS the content.
		m_x_content_w = m_x_top_w;
		m_x_content_h = m_x_top_h;
		return;
	}
	// Re-fit the child to (0, bar) .. (W, H). The runtime follows the BOUND
	// (child) window's size by polling it, so resizing the child is all a
	// resize needs; fullscreen makes the child exactly the panel-sized
	// top-level at the panel origin (INV-1.3), with no bar.
	const int off = x11_content_offset_y();
	const uint32_t cw = m_x_top_w;
	const uint32_t ch = m_x_top_h > (uint32_t)off ? m_x_top_h - (uint32_t)off : 1u;
	if (cw != m_x_content_w || ch != m_x_content_h || off != m_x_content_off_applied) {
		XMoveResizeWindow(m_x_display, m_x_content, 0, off, cw, ch);
		XFlush(m_x_display);
		m_x_content_w = cw;
		m_x_content_h = ch;
		m_x_content_off_applied = off;
		m_x_bar.invalidate();
	}
}

void
DxrLinuxWindow::x11_begin_drag(int root_x, int root_y, unsigned int button)
{
	m_x_dragging = true;
	m_x_drag_from_bar = false; // the bar's caller sets it after this returns
	m_x_drag_btn = button;
	m_x_drag_ptr_x = root_x;
	m_x_drag_ptr_y = root_y;
	int top_x = 0, top_y = 0;
	x11_root_origin(m_x_display, m_x_window, &top_x, &top_y);
	x11_root_origin(m_x_display, x11_bound_window(), &m_x_drag_origin_x, &m_x_drag_origin_y);
	m_x_drag_off_x = m_x_drag_origin_x - top_x;
	m_x_drag_off_y = m_x_drag_origin_y - top_y;
	m_x_drag_at_x = m_x_drag_origin_x;
	m_x_drag_at_y = m_x_drag_origin_y;
	m_x_drag_moves = 0;
	m_x_drag_snapped = 0;
	// Grab so motion OUTSIDE the window keeps arriving: the pointer routinely
	// leaves a window being dragged fast (and, with a click-through input
	// shape, leaves the shape on the very first pixel).
	const unsigned int motion_mask =
	    button == Button1 ? Button1MotionMask : (button == Button2 ? Button2MotionMask : Button3MotionMask);
	XGrabPointer(m_x_display, m_x_window, False, ButtonReleaseMask | PointerMotionMask | motion_mask, GrabModeAsync,
	             GrabModeAsync, None, None, CurrentTime);
	DXRW_INFO("drag: start (button %u) — grab origin (%d, %d), pointer (%d, %d)", button, m_x_drag_origin_x,
	          m_x_drag_origin_y, m_x_drag_ptr_x, m_x_drag_ptr_y);
}

void
DxrLinuxWindow::x11_end_drag()
{
	if (!m_x_dragging) {
		return;
	}
	m_x_dragging = false;
	m_x_drag_from_bar = false;
	m_x_drag_btn = 0;
	XUngrabPointer(m_x_display, CurrentTime);
	XFlush(m_x_display);
	DXRW_INFO("drag: end — %llu move(s), %llu snapped away from the raw target, origin (%d, %d) -> (%d, %d); "
	          "placement so far: %u of %u verified moves landed exactly",
	          (unsigned long long)m_x_drag_moves, (unsigned long long)m_x_drag_snapped, m_x_drag_origin_x,
	          m_x_drag_origin_y, m_x_drag_at_x, m_x_drag_at_y, m_x_probe.moves - m_x_probe.diverged,
	          m_x_probe.moves);
}

void
DxrLinuxWindow::x11_paint_bar()
{
	if (!x11_bar_visible()) {
		return;
	}
	// Plain 2D in the top-level, repainted only when it changed (hover,
	// press, focus, title, width) or the server discarded it (Expose).
	if (m_x_bar.dirty() || m_x_bar.renderedWidth() != m_x_top_w) {
		dxr_x11_chrome::Paint(m_x_bar, m_x_display, m_x_window, m_x_visual, m_x_top_w);
	}
}

bool
DxrLinuxWindow::x11_shape_available()
{
#ifdef DXR_LW_HAVE_XSHAPE
	if (m_x_shape_state == 0 && m_x_display != nullptr) {
		int ev = 0, err = 0;
		m_x_shape_state = XShapeQueryExtension(m_x_display, &ev, &err) ? 1 : -1;
		if (m_x_shape_state < 0) {
			DXRW_WARN("X11: the server has no SHAPE extension — click-through input regions are unavailable");
		}
	}
	return m_x_shape_state > 0;
#else
	if (m_x_shape_state == 0) {
		m_x_shape_state = -1;
		DXRW_WARN("X11: built without libXext (XShape) — click-through input regions are unavailable");
	}
	return false;
#endif
}

//! Map an X keysym onto the backend-neutral key identity.
static DxrKey
dxr_key_from_keysym(KeySym ks)
{
	switch (ks) {
	case XK_Escape: return DxrKey::Escape;
	case XK_q:
	case XK_Q: return DxrKey::Q;
	case XK_m:
	case XK_M: return DxrKey::M;
	case XK_o:
	case XK_O: return DxrKey::O;
	case XK_v:
	case XK_V: return DxrKey::V;
	case XK_1: return DxrKey::Num1;
	case XK_2: return DxrKey::Num2;
	case XK_3: return DxrKey::Num3;
	case XK_F11: return DxrKey::F11;
	default: return DxrKey::Unknown;
	}
}

void
DxrLinuxWindow::destroy_x11()
{
	if (m_x_display != nullptr) {
		dxr_x11_chrome::Release(m_x_display);
		if (m_x_window != 0) {
			XDestroyWindow(m_x_display, m_x_window); // takes the content child with it
			m_x_window = 0;
		}
		m_x_content = 0;
		if (m_x_colormap != 0) {
			XFreeColormap(m_x_display, m_x_colormap);
			m_x_colormap = 0;
		}
		XCloseDisplay(m_x_display);
		m_x_display = nullptr;
	}
	m_x_visual = nullptr;
	m_x_keys_down.reset();
}


/*
 *
 * Wayland leg.
 *
 */

#ifdef DXR_APP_HAVE_WAYLAND

void
DxrLinuxWindow::s_registry_global(void *data, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version)
{
	auto *self = static_cast<DxrLinuxWindow *>(data);

#ifdef DXR_APP_HAVE_WL_CHROME
	// wl_shm / wl_subcompositor / decoration manager / cursor shapes (#1654).
	if (self->m_wl_chrome.on_global(r, name, iface, version)) {
		return;
	}
#endif

	if (strcmp(iface, wl_compositor_interface.name) == 0) {
		self->m_wl_compositor = static_cast<struct wl_compositor *>(
		    wl_registry_bind(r, name, &wl_compositor_interface, version < 4 ? version : 4));
	} else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
		// v2+ is what reports the TILED_* toplevel states (the title bar squares
		// its corners for them, as GNOME does); the toplevel listener below
		// covers every event up to v5.
		self->m_wl_wm_base = static_cast<struct xdg_wm_base *>(
		    wl_registry_bind(r, name, &xdg_wm_base_interface, version < 5 ? version : 5));
		static const struct xdg_wm_base_listener kWmBaseListener = {
		    s_wm_base_ping,
		};
		xdg_wm_base_add_listener(self->m_wl_wm_base, &kWmBaseListener, self);
	} else if (strcmp(iface, wl_seat_interface.name) == 0 && self->m_wl_seat == nullptr) {
		self->m_wl_seat =
		    static_cast<struct wl_seat *>(wl_registry_bind(r, name, &wl_seat_interface, version < 5 ? version : 5));
		static const struct wl_seat_listener kSeatListener = {
		    s_seat_capabilities,
		    s_seat_name,
		};
		wl_seat_add_listener(self->m_wl_seat, &kSeatListener, self);
	} else if (strcmp(iface, wp_viewporter_interface.name) == 0) {
		self->m_wl_viewporter =
		    static_cast<struct wp_viewporter *>(wl_registry_bind(r, name, &wp_viewporter_interface, 1));
	} else if (strcmp(iface, wp_fractional_scale_manager_v1_interface.name) == 0) {
		self->m_wl_frac_manager = static_cast<struct wp_fractional_scale_manager_v1 *>(
		    wl_registry_bind(r, name, &wp_fractional_scale_manager_v1_interface, 1));
	} else if (strcmp(iface, zxdg_output_manager_v1_interface.name) == 0) {
		// #1596. Core wl_output cannot express a fractional scale, so without
		// this the app has no way to convert a logical origin into the device
		// pixels the runtime's panel rect is expressed in.
		self->m_wl_xdg_output_manager = static_cast<struct zxdg_output_manager_v1 *>(
		    wl_registry_bind(r, name, &zxdg_output_manager_v1_interface, version < 2 ? version : 2));
		// Outputs may have arrived before the manager did; give them their
		// xdg_output now rather than relying on registry ordering.
		for (auto &out : self->m_wl_outputs) {
			self->wl_attach_xdg_output(out);
		}
	} else if (strcmp(iface, wl_output_interface.name) == 0) {
		WlOutput out = {};
		out.name = name;
		out.output = static_cast<struct wl_output *>(
		    wl_registry_bind(r, name, &wl_output_interface, version < 2 ? version : 2));
		self->m_wl_outputs.push_back(out);
		static const struct wl_output_listener kOutputListener = {
		    s_output_geometry, s_output_mode, s_output_done, s_output_scale, s_output_name, s_output_description,
		};
		wl_output_add_listener(out.output, &kOutputListener, self);
		self->wl_attach_xdg_output(self->m_wl_outputs.back());
	}
}

void
DxrLinuxWindow::wl_attach_xdg_output(WlOutput &out)
{
	if (m_wl_xdg_output_manager == nullptr || out.xdg_output != nullptr || out.output == nullptr) {
		return;
	}
	out.xdg_output = zxdg_output_manager_v1_get_xdg_output(m_wl_xdg_output_manager, out.output);
	static const struct zxdg_output_v1_listener kXdgOutputListener = {
	    s_xdg_output_logical_position, s_xdg_output_logical_size,
	    s_xdg_output_done,             s_xdg_output_name,
	    s_xdg_output_description,
	};
	zxdg_output_v1_add_listener(out.xdg_output, &kXdgOutputListener, this);
}

void
DxrLinuxWindow::s_registry_global_remove(void *data, struct wl_registry *r, uint32_t name)
{
	(void)r;
	auto *self = static_cast<DxrLinuxWindow *>(data);
	// Outputs can come and go (hotplug); the surface keeps its fullscreen
	// state, so all we must do is stop tracking the record.
	for (auto it = self->m_wl_outputs.begin(); it != self->m_wl_outputs.end(); ++it) {
		if (it->name == name) {
			if (it->xdg_output != nullptr) {
				zxdg_output_v1_destroy(it->xdg_output);
			}
			self->m_wl_outputs.erase(it);
			return;
		}
	}
}

void
DxrLinuxWindow::s_wm_base_ping(void *data, struct xdg_wm_base *b, uint32_t serial)
{
	(void)data;
	// Mandatory: a client that does not pong is killed as unresponsive.
	xdg_wm_base_pong(b, serial);
}

void
DxrLinuxWindow::s_xdg_surface_configure(void *data, struct xdg_surface *s, uint32_t serial)
{
	auto *self = static_cast<DxrLinuxWindow *>(data);
	xdg_surface_ack_configure(s, serial);
	self->m_wl_configured = true;
	// The configure size may have changed: re-map the declared buffer onto it.
	// Pending state; Mesa's WSI commits it with its next present (before the
	// session, the first present is the first commit that carries a buffer).
	self->wl_apply_buffer_mapping();
#ifdef DXR_APP_HAVE_WL_CHROME
	// Window geometry (pending on the same surface, same WSI commit) and the
	// bar itself (its own desync commit) follow the new content size.
	self->m_wl_chrome.update(self->m_wl_config_w, self->m_wl_config_h, self->wl_surface_scale());
#endif
}

void
DxrLinuxWindow::s_frac_preferred_scale(void *data, struct wp_fractional_scale_v1 *f, uint32_t scale_120)
{
	(void)f;
	auto *self = static_cast<DxrLinuxWindow *>(data);
	if (scale_120 == 0 || scale_120 == self->m_wl_pref_scale_120) {
		return;
	}
	DXRW_INFO("Wayland: compositor's preferred surface scale %.4f (wp_fractional_scale_v1)",
	          (double)scale_120 / 120.0);
	self->m_wl_pref_scale_120 = scale_120;
	// Windowed start: keep the requested DEVICE-pixel size (like-for-like
	// with X11) by re-deriving the logical size from the real scale. Only
	// until the compositor sizes the window itself (a user resize).
	if (self->m_wl_size_from_desc && !self->m_wl_fullscreen && !self->m_wl_fs_deferred &&
	    self->m_desc.width > 0 && self->m_desc.height > 0) {
		const int32_t lw = (int32_t)((double)self->m_desc.width * 120.0 / (double)scale_120 + 0.5);
		const int32_t lh = (int32_t)((double)self->m_desc.height * 120.0 / (double)scale_120 + 0.5);
		if (lw != self->m_wl_config_w || lh != self->m_wl_config_h) {
			DXRW_INFO("Wayland: windowed size %dx%d -> %dx%d logical, so the buffer is the requested %ux%u "
			          "device px",
			          self->m_wl_config_w, self->m_wl_config_h, lw, lh, self->m_desc.width,
			          self->m_desc.height);
			self->m_wl_config_w = self->m_wl_windowed_w = lw;
			self->m_wl_config_h = self->m_wl_windowed_h = lh;
		}
		self->m_wl_size_from_desc = false;
	}
#ifdef DXR_APP_HAVE_WL_CHROME
	self->wl_lattice_on_placement_change(); // a drag reaching (or leaving) the 3D panel
#endif
	// A windowed surface's declared buffer is configure x this scale, so the
	// mapping (and, in pump(), the runtime's declared geometry) follows it.
	self->wl_apply_buffer_mapping();
#ifdef DXR_APP_HAVE_WL_CHROME
	// The title bar is its own surface with its own buffer: redraw it at the
	// new scale too, or it stays rasterised for the monitor the window left
	// (the right size, since its viewport destination is logical, but blurry).
	if (self->m_wl_config_w > 0 && self->m_wl_config_h > 0) {
		self->m_wl_chrome.update(self->m_wl_config_w, self->m_wl_config_h, self->wl_surface_scale());
	}
#endif
}

void
DxrLinuxWindow::s_toplevel_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *states)
{
	(void)t;
	auto *self = static_cast<DxrLinuxWindow *>(data);
#ifdef DXR_APP_HAVE_WL_CHROME
	// States first: whether the bar is shown (not when fullscreen) decides how
	// much of the size below is bar.
	self->m_wl_chrome.on_toplevel_states(states, w > 0 && h > 0);
#else
	(void)states;
#endif
	// Fullscreen state, for F11 and for the declared buffer size. Only a
	// SIZED configure may clear it: mutter's first configure after a pre-map
	// set_fullscreen is 0x0 and carries no fullscreen state yet.
	bool fs = false, maximized = false;
	if (states != nullptr) {
		const uint32_t *st = static_cast<const uint32_t *>(states->data);
		for (size_t i = 0; i < states->size / sizeof(uint32_t); i++) {
			fs |= st[i] == XDG_TOPLEVEL_STATE_FULLSCREEN;
			maximized |= st[i] == XDG_TOPLEVEL_STATE_MAXIMIZED;
		}
	}
	if (fs || (w > 0 && h > 0) || self->m_wl_unfullscreen_pending) {
		// (An unset_fullscreen WE asked for may be answered with 0x0 — the
		// compositor has no windowed size to restore for a window that
		// started fullscreen — and that answer must still count.)
		self->m_wl_unfullscreen_pending = false;
		self->wl_set_fullscreen_state(fs, &w, &h);
	}
	// 0x0 means "you choose"; keep whatever we asked for.
	if (w > 0 && h > 0) {
#ifdef DXR_APP_HAVE_WL_CHROME
		// The configure size is the WINDOW GEOMETRY, which with client-side
		// decorations is bar + content (#1654). The bound surface — and so
		// everything declared to the runtime — is the content alone.
		self->m_wl_chrome.frame_to_content(&w, &h);
#endif
		// A size the compositor chose (a user resize, maximise, tiling) ends
		// the requested-size start; an echo of our own size does not.
		if (w != self->m_wl_config_w || h != self->m_wl_config_h || fs || maximized) {
			self->m_wl_size_from_desc = false;
		}
		self->m_wl_config_w = w;
		self->m_wl_config_h = h;
		if (!fs && !maximized) {
			// Remembered so leaving fullscreen can come back to it.
			self->m_wl_windowed_w = w;
			self->m_wl_windowed_h = h;
		}
	}
}

void
DxrLinuxWindow::wl_set_fullscreen_state(bool fs, int32_t *cfg_w, int32_t *cfg_h)
{
	if (fs == m_wl_fullscreen) {
		return;
	}
	m_wl_fullscreen = fs;
#ifdef DXR_APP_HAVE_WL_CHROME
	// Authoritative for the bar too: a 0x0 unfullscreen configure carries
	// nothing the chrome could read the transition from.
	m_wl_chrome.set_fullscreen(fs);
#endif
	if (fs) {
		// On the panel output the declared buffer is the panel's MODE — the
		// 1:1 size, whatever the preferred scale says yet.
		m_wl_fullscreen_mode_w = m_wl_fs_on_panel ? m_wl_panel_mode_w : 0;
		m_wl_fullscreen_mode_h = m_wl_fs_on_panel ? m_wl_panel_mode_h : 0;
		DXRW_INFO("Wayland: now fullscreen%s", m_wl_fs_on_panel ? " — requested on the 3D panel's output" : "");
		// Judge where it actually landed once any enter/leave the move causes
		// has arrived (each of those re-arms this too).
		m_wl_output_reported.clear();
		m_wl_output_report_in = 10;
		return;
	}
	m_wl_fullscreen_mode_w = 0;
	m_wl_fullscreen_mode_h = 0;
	// Leaving fullscreen with a 0x0 configure ("you choose"): a window that
	// STARTED fullscreen has no size of its own for the compositor to restore,
	// so pick one — the last windowed size, else two thirds of the output.
	if (*cfg_w <= 0 || *cfg_h <= 0) {
		int32_t ww = m_wl_windowed_w, wh = m_wl_windowed_h;
		if (ww <= 0 || wh <= 0) {
			ww = m_wl_config_w * 2 / 3;
			wh = m_wl_config_h * 2 / 3;
		}
		m_wl_config_w = ww > 0 ? ww : 1280;
		m_wl_config_h = wh > 0 ? wh : 720;
	}
	DXRW_INFO("Wayland: left fullscreen — windowed %dx%d logical", *cfg_w > 0 ? *cfg_w : m_wl_config_w,
	          *cfg_h > 0 ? *cfg_h : m_wl_config_h);
}

void
DxrLinuxWindow::s_toplevel_close(void *data, struct xdg_toplevel *t)
{
	(void)t;
	static_cast<DxrLinuxWindow *>(data)->m_wl_close_requested = true;
}

void
DxrLinuxWindow::s_toplevel_configure_bounds(void *data, struct xdg_toplevel *t, int32_t w, int32_t h)
{
	(void)data;
	(void)t;
	(void)w;
	(void)h;
}

void
DxrLinuxWindow::s_toplevel_wm_capabilities(void *data, struct xdg_toplevel *t, struct wl_array *caps)
{
	(void)data;
	(void)t;
	(void)caps;
}

void
DxrLinuxWindow::s_output_geometry(void *data,
                                  struct wl_output *o,
                                  int32_t x,
                                  int32_t y,
                                  int32_t pw,
                                  int32_t ph,
                                  int32_t subpixel,
                                  const char *make,
                                  const char *model,
                                  int32_t transform)
{
	(void)pw;
	(void)ph;
	(void)subpixel;
	(void)make;
	(void)model;
	(void)transform;
	auto *self = static_cast<DxrLinuxWindow *>(data);
	for (auto &out : self->m_wl_outputs) {
		if (out.output == o) {
			// LOGICAL, despite the event's name. xdg_output.logical_position
			// supersedes this when the manager exists; this is the seed for a
			// compositor that has no xdg-output.
			out.logical_x = x;
			out.logical_y = y;
			return;
		}
	}
}

void
DxrLinuxWindow::s_output_mode(void *data, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t refresh)
{
	if ((flags & WL_OUTPUT_MODE_CURRENT) == 0) {
		return;
	}
	auto *self = static_cast<DxrLinuxWindow *>(data);
	for (auto &out : self->m_wl_outputs) {
		if (out.output == o) {
			// DEVICE pixels — the one core-protocol geometry that already is.
			out.mode_w = w;
			out.mode_h = h;
			// Already milli-hertz on the wire; XrWaylandSurfaceGeometryDXR
			// takes the same unit, so it passes through untouched. The runtime
			// needs it because Wayland has no XCB connection for the RandR
			// query that serves the X11 leg, and its fallback is a hardcoded
			// 60 Hz.
			out.refresh_mhz = refresh;
			return;
		}
	}
}

void
DxrLinuxWindow::s_output_done(void *data, struct wl_output *o)
{
	(void)data;
	(void)o;
}

void
DxrLinuxWindow::s_output_scale(void *data, struct wl_output *o, int32_t factor)
{
	auto *self = static_cast<DxrLinuxWindow *>(data);
	for (auto &out : self->m_wl_outputs) {
		if (out.output == o) {
			// Recorded for diagnostics ONLY — this is an integer by protocol
			// and is 2 on this box's 1.6667 output. Never a conversion factor.
			out.int_scale = factor > 0 ? factor : 1;
			return;
		}
	}
}

void
DxrLinuxWindow::s_output_name(void *data, struct wl_output *o, const char *name)
{
	(void)data;
	(void)o;
	(void)name;
}

void
DxrLinuxWindow::s_output_description(void *data, struct wl_output *o, const char *desc)
{
	(void)data;
	(void)o;
	(void)desc;
}

void
DxrLinuxWindow::s_xdg_output_logical_position(void *data, struct zxdg_output_v1 *o, int32_t x, int32_t y)
{
	auto *self = static_cast<DxrLinuxWindow *>(data);
	for (auto &out : self->m_wl_outputs) {
		if (out.xdg_output == o) {
			out.logical_x = x;
			out.logical_y = y;
			return;
		}
	}
}

void
DxrLinuxWindow::s_xdg_output_logical_size(void *data, struct zxdg_output_v1 *o, int32_t w, int32_t h)
{
	// THE missing number (#1596). Nothing in core wl_output reports an
	// output's logical SIZE, and without it `mode / logical_size` — the only
	// honest fractional scale a client can compute — is unavailable.
	auto *self = static_cast<DxrLinuxWindow *>(data);
	for (auto &out : self->m_wl_outputs) {
		if (out.xdg_output == o) {
			out.logical_w = w;
			out.logical_h = h;
			out.have_logical_size = (w > 0 && h > 0);
			return;
		}
	}
}

void
DxrLinuxWindow::s_xdg_output_done(void *data, struct zxdg_output_v1 *o)
{
	(void)data;
	(void)o;
}

void
DxrLinuxWindow::s_xdg_output_name(void *data, struct zxdg_output_v1 *o, const char *name)
{
	auto *self = static_cast<DxrLinuxWindow *>(data);
	for (auto &out : self->m_wl_outputs) {
		if (out.xdg_output == o) {
			out.name_str = name != nullptr ? name : "";
			return;
		}
	}
}

void
DxrLinuxWindow::s_surface_enter(void *data, struct wl_surface *s, struct wl_output *o)
{
	(void)s;
	auto *self = static_cast<DxrLinuxWindow *>(data);
	for (auto *e : self->m_wl_entered) {
		if (e == o) {
			return;
		}
	}
	self->m_wl_entered.push_back(o);
	// Placement changed: re-judge a few pumps from now, once any leave that
	// belongs to the same move has arrived too.
	self->m_wl_output_report_in = 10;
#ifdef DXR_APP_HAVE_WL_CHROME
	self->wl_lattice_on_placement_change(); // a drag reaching the 3D panel
#endif
}

void
DxrLinuxWindow::s_surface_leave(void *data, struct wl_surface *s, struct wl_output *o)
{
	(void)s;
	auto *self = static_cast<DxrLinuxWindow *>(data);
	for (auto it = self->m_wl_entered.begin(); it != self->m_wl_entered.end(); ++it) {
		if (*it == o) {
			self->m_wl_entered.erase(it);
			self->m_wl_output_report_in = 10;
#ifdef DXR_APP_HAVE_WL_CHROME
			self->wl_lattice_on_placement_change(); // a drag leaving the 3D panel
#endif
			return;
		}
	}
}

std::string
DxrLinuxWindow::wl_output_label(const struct wl_output *o) const
{
	if (o == nullptr) {
		return "(the compositor's choice)";
	}
	for (const auto &out : m_wl_outputs) {
		if (out.output == o) {
			char buf[128];
			snprintf(buf, sizeof(buf), "%s %dx%d at logical %d,%d",
			         out.name_str.empty() ? "(unnamed)" : out.name_str.c_str(), out.mode_w, out.mode_h,
			         out.logical_x, out.logical_y);
			return buf;
		}
	}
	return "(unknown output)";
}

std::string
DxrLinuxWindow::wl_current_output_name() const
{
	if (m_wl_entered.size() != 1) {
		return "";
	}
	for (const auto &out : m_wl_outputs) {
		if (out.output == m_wl_entered[0]) {
			return out.name_str.empty() ? "(unnamed)" : out.name_str;
		}
	}
	return "";
}

void
DxrLinuxWindow::wl_request_panel_fullscreen(const char *why)
{
	m_wl_fs_deferred = false;
	m_wl_fs_on_panel = m_wl_panel_output != nullptr;
	xdg_toplevel_set_fullscreen(m_wl_toplevel, m_wl_panel_output);
	wl_display_flush(m_wl_display);
	m_wl_output_report_in = 30; // judge the landing once it has settled
	DXRW_INFO("Wayland: set_fullscreen on %s (%s)", wl_output_label(m_wl_panel_output).c_str(), why);
}

void
DxrLinuxWindow::wl_report_fullscreen_output()
{
	// Only a fullscreen surface on exactly one output has a placement to
	// judge; a windowed one may straddle outputs legitimately.
	if (!m_wl_fullscreen || m_wl_entered.empty()) {
		return;
	}
	std::string actual;
	for (size_t i = 0; i < m_wl_entered.size(); i++) {
		actual += (i != 0 ? " + " : "") + wl_output_label(m_wl_entered[i]);
	}
	if (actual == m_wl_output_reported) {
		return;
	}
	m_wl_output_reported = actual;
	if (!m_wl_fs_on_panel || m_wl_panel_output == nullptr) {
		DXRW_INFO("Wayland: the fullscreen surface is on %s (no 3D panel output was requested)", actual.c_str());
		return;
	}
	const bool match = m_wl_entered.size() == 1 && m_wl_entered[0] == m_wl_panel_output;
	if (match) {
		DXRW_INFO("Wayland: the compositor put the fullscreen surface on %s — the requested 3D panel output "
		          "(MATCH)",
		          actual.c_str());
	} else {
		DXRW_WARN("Wayland: the compositor put the fullscreen surface on %s, but the 3D panel is %s "
		          "(MISMATCH) — the weave will not land 1:1 on the panel. Press F11 twice to re-request, or "
		          "move the window to the panel first.",
		          actual.c_str(), wl_output_label(m_wl_panel_output).c_str());
	}
}

std::string
DxrLinuxWindow::current_output_name() const
{
#ifdef DXR_APP_HAVE_WAYLAND
	if (m_backend == DxrWindowBackend::Wayland) {
		return wl_current_output_name();
	}
#endif
	return "";
}

void
DxrLinuxWindow::s_xdg_output_description(void *data, struct zxdg_output_v1 *o, const char *desc)
{
	(void)data;
	(void)o;
	(void)desc;
}

void
DxrLinuxWindow::s_seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
	auto *self = static_cast<DxrLinuxWindow *>(data);
#ifdef DXR_APP_HAVE_WL_CHROME
	self->m_wl_chrome.on_seat_capabilities(seat, caps);
#endif
	const bool has_kb = (caps & WL_SEAT_CAPABILITY_KEYBOARD) != 0;
	if (has_kb && self->m_wl_keyboard == nullptr) {
		self->m_wl_keyboard = wl_seat_get_keyboard(seat);
		static const struct wl_keyboard_listener kKbListener = {
		    s_kb_keymap, s_kb_enter, s_kb_leave, s_kb_key, s_kb_modifiers, s_kb_repeat_info,
		};
		wl_keyboard_add_listener(self->m_wl_keyboard, &kKbListener, self);
	} else if (!has_kb && self->m_wl_keyboard != nullptr) {
		wl_keyboard_release(self->m_wl_keyboard);
		self->m_wl_keyboard = nullptr;
	}
}

void
DxrLinuxWindow::s_seat_name(void *data, struct wl_seat *seat, const char *name)
{
	(void)data;
	(void)seat;
	(void)name;
}

void
DxrLinuxWindow::s_kb_keymap(void *data, struct wl_keyboard *kb, uint32_t format, int32_t fd, uint32_t size)
{
	(void)kb;
	auto *self = static_cast<DxrLinuxWindow *>(data);
#ifdef DXR_LW_HAVE_XKBCOMMON
	// The compositor's keymap turns an evdev code into the keysym the user's
	// layout gives it — so WASD is under the same fingers on AZERTY as on the
	// X11 leg, whose XLookupKeysym is layout-aware too.
	if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && fd >= 0 && size > 0) {
		void *map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
		if (map != MAP_FAILED) {
			if (self->m_wl_xkb_ctx == nullptr) {
				self->m_wl_xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
			}
			struct xkb_keymap *km = nullptr;
			if (self->m_wl_xkb_ctx != nullptr) {
				km = xkb_keymap_new_from_string(static_cast<struct xkb_context *>(self->m_wl_xkb_ctx),
				                                static_cast<const char *>(map), XKB_KEYMAP_FORMAT_TEXT_V1,
				                                XKB_KEYMAP_COMPILE_NO_FLAGS);
			}
			munmap(map, size);
			if (km != nullptr) {
				if (self->m_wl_xkb_state != nullptr) {
					xkb_state_unref(static_cast<struct xkb_state *>(self->m_wl_xkb_state));
				}
				if (self->m_wl_xkb_keymap != nullptr) {
					xkb_keymap_unref(static_cast<struct xkb_keymap *>(self->m_wl_xkb_keymap));
				}
				self->m_wl_xkb_keymap = km;
				self->m_wl_xkb_state = xkb_state_new(km);
			}
		}
	}
#else
	(void)self;
	(void)format;
	(void)size;
#endif
	// The fd is ours now and leaks one descriptor per keymap change if not
	// closed (without xkbcommon the keymap is simply unused: the built-in
	// US table decodes evdev codes).
	if (fd >= 0) {
		close(fd);
	}
}

void
DxrLinuxWindow::s_kb_enter(void *data, struct wl_keyboard *kb, uint32_t serial, struct wl_surface *s, struct wl_array *keys)
{
	(void)kb;
	(void)serial;
	(void)s;
	(void)keys;
	DxrWindowEvent ev;
	ev.type = DxrWindowEvent::Type::FocusGained;
	static_cast<DxrLinuxWindow *>(data)->m_events.push_back(ev);
}

void
DxrLinuxWindow::s_kb_leave(void *data, struct wl_keyboard *kb, uint32_t serial, struct wl_surface *s)
{
	(void)kb;
	(void)serial;
	(void)s;
	DxrWindowEvent ev;
	ev.type = DxrWindowEvent::Type::FocusLost;
	static_cast<DxrLinuxWindow *>(data)->m_events.push_back(ev);
}

void
DxrLinuxWindow::s_kb_key(void *data, struct wl_keyboard *kb, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
	(void)kb;
	(void)serial;
	auto *self = static_cast<DxrLinuxWindow *>(data);

	// `key` is a raw evdev keycode (linux/input-event-codes.h). The keysym is
	// the one at shift level 0, like the X11 leg's XLookupKeysym(ev, 0), so
	// apps see the same XK_* values on both backends. Wayland has no
	// server-side auto-repeat: a held key is one press and one release, which
	// is exactly what held-key state (WASD) wants.
	DxrWindowEvent ev;
	ev.type = state == WL_KEYBOARD_KEY_STATE_PRESSED ? DxrWindowEvent::Type::KeyDown : DxrWindowEvent::Type::KeyUp;
	ev.keycode = key + 8;
	ev.keysym = self->wl_keysym(key);
	ev.mods = self->m_mods;
	ev.time_ms = time;
	ev.x = 0;
	ev.y = 0;
	self->m_events.push_back(ev);
}

void
DxrLinuxWindow::s_kb_modifiers(void *data,
                               struct wl_keyboard *kb,
                               uint32_t serial,
                               uint32_t depressed,
                               uint32_t latched,
                               uint32_t locked,
                               uint32_t group)
{
	(void)kb;
	(void)serial;
	auto *self = static_cast<DxrLinuxWindow *>(data);
	uint32_t mods = 0;
#ifdef DXR_LW_HAVE_XKBCOMMON
	if (self->m_wl_xkb_state != nullptr) {
		auto *st = static_cast<struct xkb_state *>(self->m_wl_xkb_state);
		xkb_state_update_mask(st, depressed, latched, locked, 0, 0, group);
		const auto active = [st](const char *name) {
			return xkb_state_mod_name_is_active(st, name, XKB_STATE_MODS_EFFECTIVE) > 0;
		};
		mods |= active(XKB_MOD_NAME_SHIFT) ? DxrModShift : 0u;
		mods |= active(XKB_MOD_NAME_CTRL) ? DxrModCtrl : 0u;
		mods |= active(XKB_MOD_NAME_ALT) ? DxrModAlt : 0u;
		mods |= active(XKB_MOD_NAME_LOGO) ? DxrModSuper : 0u;
		self->m_mods = mods;
		return;
	}
#endif
	(void)group;
	// No keymap to consult: the conventional xkb real-modifier bits, which
	// every stock keymap uses (Shift 0, Control 2, Mod1 = Alt 3, Mod4 = Super 6).
	const uint32_t m = depressed | latched | locked;
	mods |= (m & (1u << 0)) ? DxrModShift : 0u;
	mods |= (m & (1u << 2)) ? DxrModCtrl : 0u;
	mods |= (m & (1u << 3)) ? DxrModAlt : 0u;
	mods |= (m & (1u << 6)) ? DxrModSuper : 0u;
	self->m_mods = mods;
}

uint32_t
DxrLinuxWindow::wl_keysym(uint32_t key) const
{
#ifdef DXR_LW_HAVE_XKBCOMMON
	if (m_wl_xkb_keymap != nullptr && m_wl_xkb_state != nullptr) {
		auto *km = static_cast<struct xkb_keymap *>(m_wl_xkb_keymap);
		auto *st = static_cast<struct xkb_state *>(m_wl_xkb_state);
		const xkb_keycode_t kc = key + 8;
		const xkb_layout_index_t layout = xkb_state_key_get_layout(st, kc);
		const xkb_keysym_t *syms = nullptr;
		const int n = xkb_keymap_key_get_syms_by_level(km, kc, layout == XKB_LAYOUT_INVALID ? 0 : layout, 0,
		                                               &syms);
		if (n > 0 && syms != nullptr) {
			return (uint32_t)syms[0]; // xkb keysyms ARE X11 keysyms
		}
	}
#endif
	// Built-in US-layout table (level 0), used without libxkbcommon or before
	// the compositor has sent a keymap.
	switch (key) {
	case KEY_ESC: return XK_Escape;
	case KEY_1: return XK_1;
	case KEY_2: return XK_2;
	case KEY_3: return XK_3;
	case KEY_4: return XK_4;
	case KEY_5: return XK_5;
	case KEY_6: return XK_6;
	case KEY_7: return XK_7;
	case KEY_8: return XK_8;
	case KEY_9: return XK_9;
	case KEY_0: return XK_0;
	case KEY_MINUS: return XK_minus;
	case KEY_EQUAL: return XK_equal;
	case KEY_BACKSPACE: return XK_BackSpace;
	case KEY_TAB: return XK_Tab;
	case KEY_Q: return XK_q;
	case KEY_W: return XK_w;
	case KEY_E: return XK_e;
	case KEY_R: return XK_r;
	case KEY_T: return XK_t;
	case KEY_Y: return XK_y;
	case KEY_U: return XK_u;
	case KEY_I: return XK_i;
	case KEY_O: return XK_o;
	case KEY_P: return XK_p;
	case KEY_LEFTBRACE: return XK_bracketleft;
	case KEY_RIGHTBRACE: return XK_bracketright;
	case KEY_ENTER: return XK_Return;
	case KEY_LEFTCTRL: return XK_Control_L;
	case KEY_A: return XK_a;
	case KEY_S: return XK_s;
	case KEY_D: return XK_d;
	case KEY_F: return XK_f;
	case KEY_G: return XK_g;
	case KEY_H: return XK_h;
	case KEY_J: return XK_j;
	case KEY_K: return XK_k;
	case KEY_L: return XK_l;
	case KEY_SEMICOLON: return XK_semicolon;
	case KEY_APOSTROPHE: return XK_apostrophe;
	case KEY_GRAVE: return XK_grave;
	case KEY_LEFTSHIFT: return XK_Shift_L;
	case KEY_BACKSLASH: return XK_backslash;
	case KEY_Z: return XK_z;
	case KEY_X: return XK_x;
	case KEY_C: return XK_c;
	case KEY_V: return XK_v;
	case KEY_B: return XK_b;
	case KEY_N: return XK_n;
	case KEY_M: return XK_m;
	case KEY_COMMA: return XK_comma;
	case KEY_DOT: return XK_period;
	case KEY_SLASH: return XK_slash;
	case KEY_RIGHTSHIFT: return XK_Shift_R;
	case KEY_KPASTERISK: return XK_KP_Multiply;
	case KEY_LEFTALT: return XK_Alt_L;
	case KEY_SPACE: return XK_space;
	case KEY_CAPSLOCK: return XK_Caps_Lock;
	case KEY_F1: return XK_F1;
	case KEY_F2: return XK_F2;
	case KEY_F3: return XK_F3;
	case KEY_F4: return XK_F4;
	case KEY_F5: return XK_F5;
	case KEY_F6: return XK_F6;
	case KEY_F7: return XK_F7;
	case KEY_F8: return XK_F8;
	case KEY_F9: return XK_F9;
	case KEY_F10: return XK_F10;
	case KEY_F11: return XK_F11;
	case KEY_F12: return XK_F12;
	case KEY_KP7: return XK_KP_7;
	case KEY_KP8: return XK_KP_8;
	case KEY_KP9: return XK_KP_9;
	case KEY_KPMINUS: return XK_KP_Subtract;
	case KEY_KP4: return XK_KP_4;
	case KEY_KP5: return XK_KP_5;
	case KEY_KP6: return XK_KP_6;
	case KEY_KPPLUS: return XK_KP_Add;
	case KEY_KP1: return XK_KP_1;
	case KEY_KP2: return XK_KP_2;
	case KEY_KP3: return XK_KP_3;
	case KEY_KP0: return XK_KP_0;
	case KEY_KPDOT: return XK_KP_Decimal;
	case KEY_KPENTER: return XK_KP_Enter;
	case KEY_RIGHTCTRL: return XK_Control_R;
	case KEY_KPSLASH: return XK_KP_Divide;
	case KEY_RIGHTALT: return XK_Alt_R;
	case KEY_HOME: return XK_Home;
	case KEY_UP: return XK_Up;
	case KEY_PAGEUP: return XK_Page_Up;
	case KEY_LEFT: return XK_Left;
	case KEY_RIGHT: return XK_Right;
	case KEY_END: return XK_End;
	case KEY_DOWN: return XK_Down;
	case KEY_PAGEDOWN: return XK_Page_Down;
	case KEY_INSERT: return XK_Insert;
	case KEY_DELETE: return XK_Delete;
	case KEY_LEFTMETA: return XK_Super_L;
	case KEY_RIGHTMETA: return XK_Super_R;
	default: return 0;
	}
}

void
DxrLinuxWindow::wl_to_buffer(double lx, double ly, int32_t *bx, int32_t *by) const
{
	// The content surface's buffer is the declared size; the pointer arrives
	// in the configure (logical) size. Their ratio is the mapping, whatever
	// produced it (fractional scale, fullscreen-on-mode, integer scale).
	uint32_t dw = 0, dh = 0;
	wl_declared_size(&dw, &dh);
	const double sx = (m_wl_config_w > 0 && dw > 0) ? (double)dw / (double)m_wl_config_w : 1.0;
	const double sy = (m_wl_config_h > 0 && dh > 0) ? (double)dh / (double)m_wl_config_h : 1.0;
	*bx = (int32_t)(lx * sx);
	*by = (int32_t)(ly * sy);
}

void
DxrLinuxWindow::s_content_pointer(void *userdata, const DxrWlPointerEvent &ev)
{
	static_cast<DxrLinuxWindow *>(userdata)->wl_content_pointer(ev);
}

void
DxrLinuxWindow::wl_content_pointer(const DxrWlPointerEvent &pe)
{
	DxrWindowEvent ev;
	ev.mods = m_mods;
	ev.time_ms = pe.time;
	switch (pe.kind) {
	case DxrWlPointerEvent::Kind::Enter:
	case DxrWlPointerEvent::Kind::Motion:
		m_wl_ptr_x = pe.x;
		m_wl_ptr_y = pe.y;
		ev.type = DxrWindowEvent::Type::Motion;
		wl_to_buffer(pe.x, pe.y, &ev.x, &ev.y);
		m_events.push_back(ev);
		return;
	case DxrWlPointerEvent::Kind::Leave:
		ev.type = DxrWindowEvent::Type::PointerLeave;
		m_events.push_back(ev);
		return;
	case DxrWlPointerEvent::Kind::Button: {
		ev.type = pe.pressed ? DxrWindowEvent::Type::ButtonDown : DxrWindowEvent::Type::ButtonUp;
		switch (pe.button) {
		case BTN_LEFT: ev.button = 1; break;
		case BTN_MIDDLE: ev.button = 2; break;
		case BTN_RIGHT: ev.button = 3; break;
		case BTN_SIDE: ev.button = 8; break;
		case BTN_EXTRA: ev.button = 9; break;
		default: return;
		}
		wl_to_buffer(pe.x, pe.y, &ev.x, &ev.y);
		// The content drag button: the compositor's own move (the same one
		// the title bar and Super+drag run), so the runtime's
		// geometry-service phase tracking is unchanged. A fullscreen window
		// is never dragged.
		if (m_desc.wayland_drag_button != 0 && ev.button == m_desc.wayland_drag_button && !m_wl_fullscreen &&
		    m_wl_toplevel != nullptr && m_wl_seat != nullptr) {
			ev.window_drag = true;
			if (pe.pressed) {
#ifdef DXR_APP_HAVE_WL_CHROME
				// The same phase-snapped drag the title bar runs (#1609):
				// hand the compositor this drag's lattice first.
				wl_drag_prepare();
#endif
				xdg_toplevel_move(m_wl_toplevel, m_wl_seat, pe.serial);
				DXRW_INFO("drag: compositor move (button %u)", ev.button);
			}
		}
		m_events.push_back(ev);
		return;
	}
	case DxrWlPointerEvent::Kind::Axis:
		m_wl_axis_time = pe.time;
		if (pe.axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
			m_wl_axis_y += pe.value;
		} else {
			m_wl_axis_x += pe.value;
		}
		return;
	case DxrWlPointerEvent::Kind::AxisDiscrete:
		m_wl_frame_discrete = true;
		if (pe.axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
			m_wl_discrete_y += pe.discrete;
		} else {
			m_wl_discrete_x += pe.discrete;
		}
		return;
	case DxrWlPointerEvent::Kind::Frame: {
		// Wheel clicks when the device reports them; otherwise (touchpads)
		// the continuous value, one step per 10 units — the compositor's own
		// "one click" distance. Wayland's positive is down / right; ours is
		// up (+1 away from the user), like X11's button 4.
		int32_t sy = 0, sx = 0;
		if (m_wl_frame_discrete) {
			sy = -m_wl_discrete_y;
			sx = m_wl_discrete_x;
			m_wl_axis_x = m_wl_axis_y = 0.0;
		} else {
			while (m_wl_axis_y >= 10.0) {
				sy -= 1;
				m_wl_axis_y -= 10.0;
			}
			while (m_wl_axis_y <= -10.0) {
				sy += 1;
				m_wl_axis_y += 10.0;
			}
			while (m_wl_axis_x >= 10.0) {
				sx += 1;
				m_wl_axis_x -= 10.0;
			}
			while (m_wl_axis_x <= -10.0) {
				sx -= 1;
				m_wl_axis_x += 10.0;
			}
		}
		m_wl_frame_discrete = false;
		m_wl_discrete_x = m_wl_discrete_y = 0;
		if (sx != 0 || sy != 0) {
			ev.type = DxrWindowEvent::Type::Scroll;
			ev.scroll_steps = sy;
			ev.scroll_steps_x = sx;
			ev.time_ms = m_wl_axis_time;
			wl_to_buffer(m_wl_ptr_x, m_wl_ptr_y, &ev.x, &ev.y);
			m_events.push_back(ev);
		}
		return;
	}
	}
}

void
DxrLinuxWindow::s_kb_repeat_info(void *data, struct wl_keyboard *kb, int32_t rate, int32_t delay)
{
	(void)data;
	(void)kb;
	(void)rate;
	(void)delay;
}

#ifdef DXR_APP_HAVE_WL_CHROME
/*
 *
 * Drag lattice (#1609).
 *
 */

namespace {
//! Half-extent of one table, LOGICAL px. A drag that goes further asks for
//! the next table (DragLatticeNeeded); this is sized so most drags never do.
constexpr int32_t kLatticeHalf = 192;
//! Grid pitch the DP is probed at, LOGICAL px. The window moves in steps no
//! coarser than this; a lens lattice denser than it is represented by the
//! nearest phase-correct point in each cell.
constexpr int32_t kLatticeCell = 3;

} // namespace

DxrLinuxWindow::LatticeProbe
DxrLinuxWindow::wl_probe_lattice(SnapWindowOriginFn fn, void *ud, const dxr_wl_lattice::Map &map, int32_t cx, int32_t cy)
{
	const auto t0 = std::chrono::steady_clock::now();
	// The snap is displacement-only: origin (0,0), target = the DEVICE
	// displacement the logical move produces. Whatever it returns preserves
	// the phase the window had at the drag start.
	auto snap = [fn, ud](int32_t tx, int32_t ty, int32_t *ox, int32_t *oy) { return fn(ud, 0, 0, tx, ty, ox, oy); };
	const dxr_wl_lattice::Probe p = dxr_wl_lattice::probe(snap, map, cx, cy, kLatticeHalf, kLatticeCell);
	LatticeProbe r;
	r.dxs = p.dxs;
	r.dys = p.dys;
	r.probed = p.probed;
	r.fixed = p.fixed;
	r.declined = p.declined;
	r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	return r;
}

bool
DxrLinuxWindow::wl_submit_lattice(bool extend, int32_t cx, int32_t cy, const LatticeProbe &r, bool async)
{
	m_wl_drag_stats.probe_ms += r.ms;
	if (r.ms > m_wl_drag_stats.probe_ms_max) {
		m_wl_drag_stats.probe_ms_max = r.ms;
	}
	if (r.declined) {
		DXRW_INFO("drag lattice: the display processor declined (no usable viewing distance) — the "
		          "compositor drags unconstrained");
		return false;
	}
	if (r.fixed == r.probed) {
		// Every probed position is already phase-correct: the lattice is
		// trivial (e.g. sim_display at its default period). A table would only
		// coarsen the drag to the probe grid for no benefit.
		if (!extend) {
			DXRW_INFO("drag lattice: the display processor accepts every position (%zu probed in %.1f ms at "
			          "scale %.4f) — nothing to constrain",
			          r.probed, r.ms, m_wl_lattice_map.scale);
		}
		return false;
	}
	// The start the table was built for: explicit when the publisher takes
	// one (v8) — a table at a fractional scale is only valid for its start.
	const int32_t start[2] = {m_wl_lattice_start_frame_x, m_wl_lattice_start_frame_y};
	const bool ok = m_wl_placement.set_drag_lattice(
	    extend, kLatticeCell, cx - kLatticeHalf, cy - kLatticeHalf, cx + kLatticeHalf, cy + kLatticeHalf, r.dxs,
	    r.dys, &m_wl_lattice_start_x, &m_wl_lattice_start_y, m_wl_lattice_explicit ? start : nullptr);
	if (ok) {
		if (extend) {
			m_wl_drag_stats.extensions++;
		} else {
			m_wl_drag_stats.tables++;
		}
	}
	DXRW_INFO("drag lattice: %s %zu phase-correct reachable displacement(s) around (%+d, %+d) logical — %zu "
	          "probed in %.1f ms at scale %.4f%s%s",
	          extend ? "extended with" : "sent", r.dxs.size(), cx, cy, r.probed, r.ms, m_wl_lattice_map.scale,
	          async ? " (worker thread)" : "", ok ? "" : " — REFUSED by the compositor, dragging unconstrained");
	return ok;
}

bool
DxrLinuxWindow::wl_send_lattice(bool extend, int32_t cx, int32_t cy)
{
	const LatticeProbe r = wl_probe_lattice(m_snap_fn, m_snap_userdata, m_wl_lattice_map, cx, cy);
	return wl_submit_lattice(extend, cx, cy, r, false);
}

void
DxrLinuxWindow::wl_request_lattice_async(bool extend, int32_t cx, int32_t cy)
{
	// DXR_WL_LATTICE_SYNC=1: probe on this thread (the pre-follow-up
	// behaviour), in case a display processor turns out not to tolerate a
	// snap query from a second thread.
	static const bool sync = [] {
		const char *e = getenv("DXR_WL_LATTICE_SYNC");
		return e != nullptr && e[0] == '1';
	}();
	if (sync) {
		const bool ok = wl_send_lattice(extend, cx, cy);
		if (!extend) {
			m_wl_lattice_active = ok;
		}
		return;
	}
	if (m_wl_lattice_job_running) {
		// One at a time; the newest request is the one worth answering.
		// A queued FRESH table is never downgraded to an extension: it is
		// what establishes the drag's origin.
		if (m_wl_lattice_req_pending && !m_wl_lattice_req_extend && extend) {
			return;
		}
		m_wl_lattice_req_pending = true;
		m_wl_lattice_req_extend = extend;
		m_wl_lattice_req_cx = cx;
		m_wl_lattice_req_cy = cy;
		return;
	}
	m_wl_lattice_job_running = true;
	m_wl_lattice_job_done.store(false);
	m_wl_lattice_job_extend = extend;
	m_wl_lattice_job_cx = cx;
	m_wl_lattice_job_cy = cy;
	m_wl_drag_stats.async_jobs++;
	SnapWindowOriginFn fn = m_snap_fn;
	void *ud = m_snap_userdata;
	const dxr_wl_lattice::Map map = m_wl_lattice_map;
	m_wl_lattice_thread = std::thread([this, fn, ud, map, cx, cy] {
		m_wl_lattice_job_result = wl_probe_lattice(fn, ud, map, cx, cy);
		m_wl_lattice_job_done.store(true);
	});
}

void
DxrLinuxWindow::wl_poll_lattice_job()
{
	if (!m_wl_lattice_job_running || !m_wl_lattice_job_done.load()) {
		return;
	}
	m_wl_lattice_thread.join();
	m_wl_lattice_job_running = false;
	const bool extend = m_wl_lattice_job_extend;
	// The world may have moved on while the worker probed: the drag ended,
	// or the window left the panel. A late table would only confuse.
	if (!m_wl_compositor_drag || (extend && !m_wl_lattice_active) || !m_wl_lattice_map_valid) {
		m_wl_lattice_req_pending = false;
		return;
	}
	const bool ok = wl_submit_lattice(extend, m_wl_lattice_job_cx, m_wl_lattice_job_cy, m_wl_lattice_job_result,
	                                  true);
	if (!extend) {
		m_wl_lattice_active = ok;
	}
	if (m_wl_lattice_req_pending) {
		m_wl_lattice_req_pending = false;
		wl_request_lattice_async(m_wl_lattice_req_extend, m_wl_lattice_req_cx, m_wl_lattice_req_cy);
	}
}

bool
DxrLinuxWindow::wl_window_on_panel() const
{
	if (m_wl_panel_output == nullptr) {
		return true; // no panel identified (one monitor, or a test): anywhere counts
	}
	for (auto *o : m_wl_entered) {
		if (o == m_wl_panel_output) {
			return true;
		}
	}
	return false;
}

bool
DxrLinuxWindow::wl_lattice_prepare_map(bool quiet)
{
	m_wl_lattice_map_valid = false;
	m_wl_lattice_explicit = false;
	/*
	 * ANY output scale (runtime#1609). The table is built over LOGICAL
	 * displacements, each mapped to the device displacement Mutter actually
	 * produces for it — see dxr_wl_lattice.h. That mapping depends on where
	 * the drag starts (the rounding), so the start is read from the geometry
	 * service and handed back with the table (SetDragLatticeAt, v8).
	 */
	DxrWlPlacement::OwnGeometry g;
	if (m_wl_placement.has_explicit_start() && m_wl_placement.get_own_geometry(&g)) {
		// The window's monitor must be the panel: the rounding is relative to
		// the monitor the window is drawn on, and the lattice only matters on
		// the panel.
		if (m_wl_panel_output != nullptr) {
			for (const auto &out : m_wl_outputs) {
				if (out.output == m_wl_panel_output && out.have_logical_size &&
				    (out.logical_x != g.monitor[0] || out.logical_y != g.monitor[1])) {
					if (!quiet) {
						DXRW_INFO("drag lattice: the window is on another monitor than the 3D panel — "
						          "unconstrained until it reaches the panel");
					}
					return false;
				}
			}
		}
		m_wl_lattice_map.rel0_x = g.buffer[0] - g.monitor[0];
		m_wl_lattice_map.rel0_y = g.buffer[1] - g.monitor[1];
		m_wl_lattice_map.scale = g.monitor_scale;
		m_wl_lattice_start_frame_x = g.frame[0];
		m_wl_lattice_start_frame_y = g.frame[1];
		m_wl_lattice_explicit = true;
		m_wl_lattice_map_valid = true;
		return true;
	}
	// An older publisher (v7 and before) takes no start, so only an INTEGER
	// scale is safe: there the device displacement of a logical move does not
	// depend on where the move starts.
	const double scale = wl_surface_scale();
	const double nearest = (double)(int32_t)(scale + 0.5);
	if (scale < 1.0 || (scale > nearest ? scale - nearest : nearest - scale) > 0.01) {
		if (!quiet) {
			DXRW_INFO("drag lattice: output scale %.4f is fractional and the window-geometry extension is older "
			          "than version 8 (no explicit drag start) — unconstrained. Update the extension to "
			          "phase-snap at any scale.",
			          scale);
		}
		return false;
	}
	m_wl_lattice_map = dxr_wl_lattice::Map{0, 0, nearest};
	m_wl_lattice_map_valid = true;
	return true;
}

void
DxrLinuxWindow::wl_lattice_on_placement_change()
{
	if (!m_wl_compositor_drag || !m_wl_placement.has_drag_lattice() || m_snap_fn == nullptr) {
		return;
	}
	// A drag that had no table never gets a DragLatticeDone to end it, so
	// bound how long "a drag is running" is believed.
	if (std::chrono::steady_clock::now() - m_wl_drag_began > std::chrono::seconds(60)) {
		m_wl_compositor_drag = false;
		return;
	}
	const char *env = getenv("DXR_WL_DRAG_LATTICE");
	if (env != nullptr && env[0] == '0') {
		return;
	}
	const bool on_panel = wl_window_on_panel();
	if (on_panel && !m_wl_lattice_active && !m_wl_lattice_job_running) {
		/*
		 * The drag began off the panel (or before the window's monitor was
		 * the panel) and the window has reached it. Hardware showed exactly
		 * this case stuttering on the panel for the rest of the drag. Build a
		 * FRESH table there, for the window's position NOW — which is also the
		 * start handed back with it, so the window moving on while the worker
		 * probes does not matter.
		 */
		if (!wl_lattice_prepare_map(true)) {
			return; // e.g. still mostly on the other monitor: retried from the pump
		}
		m_wl_drag_stats.entered_mid_drag = true;
		DXRW_INFO("drag lattice: the window reached the 3D panel mid-drag (scale %.4f) — deriving a table there",
		          m_wl_lattice_map.scale);
		wl_request_lattice_async(false, 0, 0);
	} else if (!on_panel && m_wl_lattice_active) {
		// Left the panel: its lattice means nothing on this output.
		m_wl_placement.clear_drag_lattice();
		m_wl_lattice_active = false;
		m_wl_drag_stats.clears++;
		DXRW_INFO("drag lattice: the window left the 3D panel mid-drag — table dropped");
	}
}

void
DxrLinuxWindow::wl_drag_prepare()
{
	m_wl_lattice_active = false;
	// Every caller starts a compositor move right after this, table or not:
	// a drag that begins off the panel may still need one when it gets there.
	m_wl_compositor_drag = true;
	m_wl_drag_began = std::chrono::steady_clock::now();
	m_wl_drag_stats = LatticeDragStats{};
	// On by default whenever the geometry extension offers it (version 6+),
	// since hardware confirmed it (stable 3D while dragging, native drag feel).
	// DXR_WL_DRAG_LATTICE=0 turns it off; the compositor drag without a table
	// is exactly the pre-#1609 behaviour.
	const char *env = getenv("DXR_WL_DRAG_LATTICE");
	if (env != nullptr && env[0] == '0') {
		return;
	}
	if (!m_wl_placement.has_drag_lattice() || m_snap_fn == nullptr) {
		return;
	}
	if (m_wl_chrome.maximized()) {
		return; // the compositor unmaximises under the pointer; nothing to snap
	}
	if (!wl_window_on_panel() || !wl_lattice_prepare_map(false)) {
		m_wl_drag_stats.began_off_lattice = true;
		return; // the mid-drag path sends one if the window reaches the panel
	}
	m_wl_lattice_active = wl_send_lattice(false, 0, 0);
}
#endif // DXR_APP_HAVE_WL_CHROME

bool
DxrLinuxWindow::create_wayland(const DxrLinuxWindowDesc &desc)
{
	m_wl_display = wl_display_connect(nullptr);
	if (m_wl_display == nullptr) {
		DXRW_ERROR("wl_display_connect failed — is WAYLAND_DISPLAY set?");
		return false;
	}

	m_wl_registry = wl_display_get_registry(m_wl_display);
	static const struct wl_registry_listener kRegistryListener = {
	    s_registry_global,
	    s_registry_global_remove,
	};
	wl_registry_add_listener(m_wl_registry, &kRegistryListener, this);
#ifdef DXR_APP_HAVE_WL_CHROME
	// The seat's one wl_pointer is the chrome's; it forwards what happens on
	// the content surface here, as input events.
	m_wl_chrome.set_content_pointer_sink(s_content_pointer, this);
#endif

	// First roundtrip: globals. Second: the per-output geometry/mode/scale
	// bursts the first one only triggered. Third: the zxdg_output_v1 bursts
	// (#1596) — an xdg_output created during the registry callback, or during
	// the manager's own bind, only answers on the NEXT round trip, and without
	// its logical_size there is no fractional scale and therefore no
	// device-pixel panel match.
	wl_display_roundtrip(m_wl_display);
	wl_display_roundtrip(m_wl_display);
	wl_display_roundtrip(m_wl_display);

	if (m_wl_compositor == nullptr) {
		DXRW_ERROR("Wayland compositor advertises no wl_compositor");
		return false;
	}
	if (m_wl_wm_base == nullptr) {
		DXRW_ERROR("Wayland compositor advertises no xdg_wm_base — xdg-shell is required");
		return false;
	}

	m_wl_surface = wl_compositor_create_surface(m_wl_compositor);
	if (m_wl_surface == nullptr) {
		DXRW_ERROR("wl_compositor_create_surface failed");
		return false;
	}
	// enter / leave: which output(s) the surface is on. The first enter is
	// also the proof that the surface is MAPPED (the runtime's WSI committed
	// a buffer), which is what the deferred fullscreen request waits for.
	// (The WSI never listens on the app's wl_surface; it uses a wrapper.)
	{
		static const struct wl_surface_listener kSurfaceListener = {
		    s_surface_enter,
		    s_surface_leave,
		};
		wl_surface_add_listener(m_wl_surface, &kSurfaceListener, this);
	}
	if (m_wl_viewporter != nullptr) {
		m_wl_viewport = wp_viewporter_get_viewport(m_wl_viewporter, m_wl_surface);
	}
	if (m_wl_frac_manager != nullptr) {
		m_wl_frac = wp_fractional_scale_manager_v1_get_fractional_scale(m_wl_frac_manager, m_wl_surface);
		static const struct wp_fractional_scale_v1_listener kFracListener = {
		    s_frac_preferred_scale,
		};
		wp_fractional_scale_v1_add_listener(m_wl_frac, &kFracListener, this);
	}
	DXRW_INFO("Wayland: buffer mapping via %s; preferred scale via %s",
	          m_wl_viewport != nullptr ? "wp_viewport destination"
	                                   : "wl_surface.set_buffer_scale (integer scales only — wp_viewporter absent)",
	          m_wl_frac != nullptr ? "wp_fractional_scale_v1" : "nothing (windowed surfaces assume scale 1)");

	m_wl_xdg_surface = xdg_wm_base_get_xdg_surface(m_wl_wm_base, m_wl_surface);
	static const struct xdg_surface_listener kXdgSurfaceListener = {
	    s_xdg_surface_configure,
	};
	xdg_surface_add_listener(m_wl_xdg_surface, &kXdgSurfaceListener, this);

	m_wl_toplevel = xdg_surface_get_toplevel(m_wl_xdg_surface);
	static const struct xdg_toplevel_listener kToplevelListener = {
	    s_toplevel_configure,
	    s_toplevel_close,
	    s_toplevel_configure_bounds,
	    s_toplevel_wm_capabilities,
	};
	xdg_toplevel_add_listener(m_wl_toplevel, &kToplevelListener, this);
	xdg_toplevel_set_title(m_wl_toplevel, desc.title);
	xdg_toplevel_set_app_id(m_wl_toplevel, desc.app_id);

	m_wl_config_w = (int32_t)desc.width;
	m_wl_config_h = (int32_t)desc.height;

	// The panel's wl_output is resolved whether or not the window starts
	// fullscreen: F11 (toggle_fullscreen) fullscreens onto it later.
	{
		/*
		 * INV-1.3 substitute. A Wayland client cannot place itself, so the
		 * only way to land on the 3D panel is to go fullscreen on the
		 * wl_output that IS the panel.
		 *
		 * The match is made in DEVICE PIXELS (#1596). It used to compare
		 * `wl_output.geometry`'s LOGICAL origin against the runtime's
		 * device-pixel panel rect, which on a scaled desktop can never
		 * succeed: on the measured box the 3D panel sits at logical x=1728 and
		 * device x=3456, so the comparison was 1728 == 3456 and the app
		 * fullscreened on whatever output the compositor chose — the laptop.
		 *
		 * SIZE is the reliable half and needs no conversion at all: an
		 * output's `wl_output.mode` is device pixels by protocol and the
		 * runtime's `displayPixelWidth/Height` is device pixels by contract.
		 * The converted ORIGIN is the corroborating half, and it is only
		 * required as a TIE-BREAK, because a single size match is already
		 * unambiguous and the two coordinate spaces can legitimately disagree
		 * (an XWayland root scales the whole layout by one integer factor
		 * while each output has its own). Same rule, and the same reason, as
		 * the runtime's own `OS_DISPLAY_DESKTOP_RULE_PIXEL_MATCH`.
		 */
		struct wl_output *chosen = nullptr;
		if (desc.panel_width != 0 && desc.panel_height != 0) {
			const WlOutput *size_match = nullptr;
			const WlOutput *exact_match = nullptr;
			size_t size_match_count = 0;
			bool any_logical_size = false;

			for (const auto &out : m_wl_outputs) {
				// mode / logical_size, never the integer wl_output.scale.
				// With no xdg_output the scale is unresolvable and only the
				// size half of the match can run — still device-vs-device,
				// still correct, just without the origin tie-break.
				struct u_wl_monitor mon = {};
				mon.logical_x = out.logical_x;
				mon.logical_y = out.logical_y;
				mon.logical_w = out.have_logical_size ? out.logical_w : 0;
				mon.logical_h = out.have_logical_size ? out.logical_h : 0;
				mon.mode_w = out.mode_w;
				mon.mode_h = out.mode_h;
				any_logical_size |= out.have_logical_size;

				bool origin_agrees = false;
				if (!u_wl_monitor_is_panel(&mon, desc.panel_left, desc.panel_top, desc.panel_width,
				                           desc.panel_height, &origin_agrees)) {
					continue;
				}
				size_match_count++;
				if (size_match == nullptr) {
					size_match = &out;
				}
				if (origin_agrees && exact_match == nullptr) {
					exact_match = &out;
				}
			}

			const WlOutput *picked = exact_match != nullptr ? exact_match : nullptr;
			if (picked == nullptr && size_match_count == 1) {
				picked = size_match;
			}

			if (picked != nullptr) {
				chosen = picked->output;
				m_wl_refresh_mhz = picked->refresh_mhz > 0 ? (uint32_t)picked->refresh_mhz : 0;
				// Device-pixel mode size — the buffer size that lands 1:1 on
				// this output. See m_wl_fullscreen_mode_w in the header.
				m_wl_fullscreen_mode_w = picked->mode_w;
				m_wl_fullscreen_mode_h = picked->mode_h;

				if (exact_match != nullptr) {
					DXRW_INFO("Wayland: found the wl_output matching the 3D panel rect "
					          "%ux%u+%d+%d in device pixels (logical origin %d,%d)",
					          desc.panel_width, desc.panel_height, desc.panel_left,
					          desc.panel_top, picked->logical_x, picked->logical_y);
				} else {
					DXRW_WARN("Wayland: one wl_output is the 3D panel's size (%ux%u device px) but "
					          "its converted origin does not match the runtime's %d,%d — taking it "
					          "anyway, since the size match is unambiguous. The two coordinate "
					          "spaces disagreeing is expected when the runtime resolved the panel "
					          "through XWayland's RandR view of the layout.",
					          desc.panel_width, desc.panel_height, desc.panel_left, desc.panel_top);
				}
			} else if (size_match_count > 1) {
				DXRW_WARN("Wayland: %zu wl_outputs are %ux%u device px and none has the runtime's "
				          "origin %d,%d — cannot tell which is the 3D panel, so going fullscreen on "
				          "the compositor's choice. INV-1.3 placement is not guaranteed.",
				          size_match_count, desc.panel_width, desc.panel_height, desc.panel_left,
				          desc.panel_top);
			} else {
				DXRW_WARN("Wayland: no wl_output is %ux%u device pixels (%zu output(s) seen%s) — "
				          "going fullscreen on the compositor's choice. INV-1.3 placement is not "
				          "guaranteed, and the runtime will refuse to weave into the resample that "
				          "follows (#1595).",
				          desc.panel_width, desc.panel_height, m_wl_outputs.size(),
				          any_logical_size ? "" : ", none reporting a logical size — is "
				                                  "zxdg_output_manager_v1 advertised?");
			}
		}
		m_wl_panel_output = chosen;
		m_wl_panel_mode_w = m_wl_fullscreen_mode_w;
		m_wl_panel_mode_h = m_wl_fullscreen_mode_h;
	}
	if (desc.fullscreen_on_wayland && m_wl_panel_output != nullptr) {
		// DEFERRED (see m_wl_fs_deferred): mutter would drop the output of a
		// pre-map request and fullscreen on whatever monitor is "current".
		// Map windowed first, at the panel's LOGICAL size, and ask once the
		// first buffer is on screen. The declared buffer is the panel's MODE
		// from the start (m_wl_fullscreen_mode_w/h stay set), so the
		// swapchain the runtime creates at xrCreateSession is already the
		// fullscreen one and the transition does not resize it.
		m_wl_fs_deferred = true;
		m_wl_fullscreen = false;
		m_wl_fs_on_panel = true;
		for (const auto &out : m_wl_outputs) {
			if (out.output == m_wl_panel_output && out.have_logical_size) {
				m_wl_config_w = out.logical_w;
				m_wl_config_h = out.logical_h;
			}
		}
		DXRW_INFO("Wayland: fullscreen on %s requested — DEFERRED until the surface is mapped (mutter "
		          "discards the output of a set_fullscreen made before the first buffer); expect one "
		          "windowed frame first",
		          wl_output_label(m_wl_panel_output).c_str());
	} else if (desc.fullscreen_on_wayland) {
		// No panel output matched: there is no output to lose, so the old
		// pre-map request is as good as any — the compositor's choice.
		xdg_toplevel_set_fullscreen(m_wl_toplevel, nullptr);
		m_wl_fullscreen = true;
		m_wl_fs_on_panel = false;
	} else {
		// Not fullscreen (yet): the declared size is configure x scale.
		m_wl_fullscreen_mode_w = 0;
		m_wl_fullscreen_mode_h = 0;
		// desc.width/height are DEVICE pixels, as on X11, so the same request
		// gives the same buffer on both backends. The surface's preferred
		// scale is only known once it is on an output, so start from the 3D
		// panel's scale (where the app aims to be) and correct on the first
		// wp_fractional_scale_v1.preferred_scale (s_frac_preferred_scale).
		double est = 1.0;
		for (const auto &out : m_wl_outputs) {
			if (out.output == m_wl_panel_output && out.have_logical_size && out.logical_w > 0) {
				est = (double)out.mode_w / (double)out.logical_w;
			}
		}
		if (m_wl_frac_manager == nullptr) {
			// No preferred scale will ever arrive, so the declared buffer is the
			// configure size itself (wl_declared_size): logical = device.
			est = 1.0;
		}
		m_wl_config_w = (int32_t)((double)desc.width / est + 0.5);
		m_wl_config_h = (int32_t)((double)desc.height / est + 0.5);
		m_wl_size_from_desc = m_wl_frac_manager != nullptr;
		m_wl_windowed_w = m_wl_config_w;
		m_wl_windowed_h = m_wl_config_h;
		DXRW_INFO("Wayland: requested %ux%u device px -> %dx%d logical at an estimated scale %.4f "
		          "(corrected by the surface's preferred scale once it is on an output)",
		          desc.width, desc.height, m_wl_config_w, m_wl_config_h, est);
		// Windowed is supported from extension spec v2: the size below is
		// declared through XrWaylandSurfaceGeometryDXR, so the runtime sizes
		// its swapchain to this surface instead of resizing it to the panel.
		// What windowed still needs, and fullscreen does not, is the
		// compositor geometry service for the weave PHASE — Wayland never
		// tells a client where its surface is.
		DXRW_WARN("Wayland: windowed mode — the surface size is declared to the runtime "
		          "(XrWaylandSurfaceGeometryDXR), so the swapchain follows the window. The weave PHASE still "
		          "needs the window-geometry service (window-geometry@displayxr.org); without it the runtime "
		          "weaves display-scoped, which is wrong for a window that is not at the panel origin.");
	}

#ifdef DXR_APP_HAVE_WL_CHROME
	// Title bar (#1654). Before the commit below: the chrome subsurface's
	// position is state of THIS surface and rides on that commit, and the
	// server-side-decoration request must precede the initial configure.
	m_wl_chrome.attach(m_wl_display, m_wl_compositor, m_wl_surface, m_wl_xdg_surface, m_wl_toplevel,
	                   m_wl_viewporter, desc.title, desc.fullscreen_on_wayland);
	if (!desc.wayland_title_bar) {
		m_wl_chrome.set_hidden(true); // before the first configure: never shown
		DXRW_INFO("Wayland: title bar hidden at start (desc.wayland_title_bar = false)");
	}
	if (m_transparent_bg) {
		m_wl_chrome.set_suppressed(true);
		DXRW_INFO("Wayland: title bar hidden at start (transparent background)");
	}
	// Drag lattice (#1609): probed once here so a press never has to find out.
	m_wl_placement.connect();
	DXRW_INFO("drag lattice: compositor placement service — %s", m_wl_placement.describe());
	m_wl_chrome.set_drag_prepare([this] { wl_drag_prepare(); });
#endif

	// The role is attached and the state requested; commit so the compositor
	// sends the initial configure. NOTE: this is the ONLY commit this helper
	// ever performs — once the session exists, Mesa's WSI owns attach/damage/
	// commit on this surface and a second commit from the app is a bug.
	wl_surface_commit(m_wl_surface);

	// Block until the initial xdg_surface.configure has been acked. Doing this
	// BEFORE xrCreateSession is mandatory: the runtime calls
	// vkCreateWaylandSurfaceKHR synchronously inside the session create
	// (comp_vk_native_target.cpp:1797) and the WSI attaches a buffer at the
	// first present — attaching to a role-less or unconfigured surface is a
	// protocol error that kills the connection.
	for (int i = 0; i < 100 && !m_wl_configured; i++) {
		if (wl_display_roundtrip(m_wl_display) < 0) {
			DXRW_ERROR("Wayland connection error while waiting for the initial configure");
			return false;
		}
	}
	if (!m_wl_configured) {
		DXRW_ERROR("No xdg_surface.configure after 100 roundtrips — giving up");
		return false;
	}

	// Refresh for the geometry struct. Fullscreen took it from the matched
	// output above; windowed has no wl_surface.enter listener here, so fall
	// back to the first output that reported a mode. Wrong only on a
	// mixed-refresh multi-head desktop, and 0 (unknown) is always safe — the
	// runtime then keeps its 60 Hz default.
	if (m_wl_refresh_mhz == 0) {
		for (const auto &out : m_wl_outputs) {
			if (out.refresh_mhz > 0) {
				m_wl_refresh_mhz = (uint32_t)out.refresh_mhz;
				break;
			}
		}
	}

	{
		uint32_t dw = 0, dh = 0;
		wl_declared_size(&dw, &dh);
		if ((int32_t)dw != m_wl_config_w || (int32_t)dh != m_wl_config_h) {
			DXRW_WARN("Wayland: declaring a %ux%u BUFFER for a %dx%d logical surface — the desktop is "
			          "fractionally scaled (%.3fx). The buffer is the output's mode size, so it still "
			          "lands 1:1 on the panel; the configure size would have been upscaled.",
			          dw, dh, m_wl_config_w, m_wl_config_h,
			          m_wl_config_w > 0 ? (double)dw / (double)m_wl_config_w : 0.0);
		}
	}

	DXRW_INFO("Created app-owned Wayland surface %p (xdg toplevel, %s), content size %dx%d @ %u mHz, "
	          "decorations %s",
	          (void *)m_wl_surface,
	          m_wl_fs_deferred ? "fullscreen on the panel once mapped"
	                           : (desc.fullscreen_on_wayland ? "fullscreen" : "windowed"),
	          m_wl_config_w,
	          m_wl_config_h, m_wl_refresh_mhz,
#ifdef DXR_APP_HAVE_WL_CHROME
	          m_wl_chrome.mode_name()
#else
	          "none (built without displayxr::csd)"
#endif
	);

	// A fullscreen surface only weaves 1:1 when the buffer we declare covers
	// the panel exactly. It normally does (the declared size is the matched
	// output's mode), so this fires when no output matched — then the buffer
	// is the LOGICAL configure size and the compositor will resample it.
	// One WARN, at create time — never per frame.
	if (desc.fullscreen_on_wayland && desc.panel_width > 0 && desc.panel_height > 0) {
		uint32_t dw = 0, dh = 0;
		wl_declared_size(&dw, &dh);
		if (dw != desc.panel_width || dh != desc.panel_height) {
			DXRW_WARN("Wayland: fullscreen buffer will be %ux%u but the 3D panel is %ux%u — no wl_output "
			          "matched, so the declared size is the LOGICAL configure size and the compositor "
			          "will resample it. The weave CANNOT be 1:1 in this session; expect the runtime "
			          "to present flat 2D rather than weave into the resample (look for its NOT_1TO1 "
			          "line, #1595). This warning is advisory — the app cannot refuse on the "
			          "runtime's behalf, and no longer has to.",
			          dw, dh, desc.panel_width, desc.panel_height);
		}
	}

	return true;
}

void
DxrLinuxWindow::destroy_wayland()
{
#ifdef DXR_APP_HAVE_WL_CHROME
	if (m_wl_lattice_thread.joinable()) {
		m_wl_lattice_thread.join(); // a probe in flight: bounded (~100 ms)
	}
	m_wl_lattice_job_running = false;
	// Before the content surface: the chrome is its subsurface.
	m_wl_chrome.destroy();
#endif
	if (m_wl_frac != nullptr) {
		wp_fractional_scale_v1_destroy(m_wl_frac);
		m_wl_frac = nullptr;
	}
	if (m_wl_viewport != nullptr) {
		wp_viewport_destroy(m_wl_viewport);
		m_wl_viewport = nullptr;
	}
	if (m_wl_frac_manager != nullptr) {
		wp_fractional_scale_manager_v1_destroy(m_wl_frac_manager);
		m_wl_frac_manager = nullptr;
	}
	if (m_wl_viewporter != nullptr) {
		wp_viewporter_destroy(m_wl_viewporter);
		m_wl_viewporter = nullptr;
	}
	if (m_wl_toplevel != nullptr) {
		xdg_toplevel_destroy(m_wl_toplevel);
		m_wl_toplevel = nullptr;
	}
	if (m_wl_xdg_surface != nullptr) {
		xdg_surface_destroy(m_wl_xdg_surface);
		m_wl_xdg_surface = nullptr;
	}
	if (m_wl_surface != nullptr) {
		wl_surface_destroy(m_wl_surface);
		m_wl_surface = nullptr;
	}
	if (m_wl_keyboard != nullptr) {
		wl_keyboard_release(m_wl_keyboard);
		m_wl_keyboard = nullptr;
	}
#ifdef DXR_LW_HAVE_XKBCOMMON
	if (m_wl_xkb_state != nullptr) {
		xkb_state_unref(static_cast<struct xkb_state *>(m_wl_xkb_state));
		m_wl_xkb_state = nullptr;
	}
	if (m_wl_xkb_keymap != nullptr) {
		xkb_keymap_unref(static_cast<struct xkb_keymap *>(m_wl_xkb_keymap));
		m_wl_xkb_keymap = nullptr;
	}
	if (m_wl_xkb_ctx != nullptr) {
		xkb_context_unref(static_cast<struct xkb_context *>(m_wl_xkb_ctx));
		m_wl_xkb_ctx = nullptr;
	}
#endif
	if (m_wl_seat != nullptr) {
		wl_seat_destroy(m_wl_seat);
		m_wl_seat = nullptr;
	}
	for (auto &out : m_wl_outputs) {
		if (out.xdg_output != nullptr) {
			zxdg_output_v1_destroy(out.xdg_output);
			out.xdg_output = nullptr;
		}
		if (out.output != nullptr) {
			wl_output_destroy(out.output);
		}
	}
	m_wl_outputs.clear();
	m_wl_entered.clear();
	m_wl_fs_deferred = false;
	m_wl_output_reported.clear();
	m_wl_output_report_in = -1;
	m_wl_pumps = 0;
	if (m_wl_xdg_output_manager != nullptr) {
		zxdg_output_manager_v1_destroy(m_wl_xdg_output_manager);
		m_wl_xdg_output_manager = nullptr;
	}
	if (m_wl_wm_base != nullptr) {
		xdg_wm_base_destroy(m_wl_wm_base);
		m_wl_wm_base = nullptr;
	}
	if (m_wl_compositor != nullptr) {
		wl_compositor_destroy(m_wl_compositor);
		m_wl_compositor = nullptr;
	}
	if (m_wl_registry != nullptr) {
		wl_registry_destroy(m_wl_registry);
		m_wl_registry = nullptr;
	}
	if (m_wl_display != nullptr) {
		wl_display_disconnect(m_wl_display);
		m_wl_display = nullptr;
	}
}

#endif // DXR_APP_HAVE_WAYLAND


/*
 *
 * Public surface.
 *
 */

DxrLinuxWindow::~DxrLinuxWindow()
{
	destroy();
}

bool
DxrLinuxWindow::create(DxrWindowBackend backend, const DxrLinuxWindowDesc &desc)
{
	m_desc = desc;
	m_backend = backend;

	m_events.clear();
	m_reported_w = m_reported_h = 0;
	m_stats_period = s_frame_stats_request;
	if (const char *e = getenv("DXR_FRAME_STATS")) {
		const double v = atof(e);
		m_stats_period = v > 0.0 ? v : (e[0] == '1' ? 5.0 : m_stats_period);
	}
	m_stats_last_ns = m_stats_window_start_ns = 0;
	m_stats_ms.clear();

	if (backend == DxrWindowBackend::X11) {
		if (!create_x11(desc)) {
			destroy_x11();
			m_backend = DxrWindowBackend::Auto;
			return false;
		}
		verify_connection(backend);
		note_content_size();
		return true;
	}

#ifdef DXR_APP_HAVE_WAYLAND
	if (backend == DxrWindowBackend::Wayland) {
		m_transparent = desc.transparent; // native on Wayland: no visual to find
		m_transparent_bg = desc.transparent && desc.transparent_background;
		if (!create_wayland(desc)) {
			destroy_wayland();
			m_backend = DxrWindowBackend::Auto;
			return false;
		}
		verify_connection(backend);
		note_content_size();
		return true;
	}
#endif

	DXRW_ERROR("DxrLinuxWindow::create called with an unresolved backend");
	m_backend = DxrWindowBackend::Auto;
	return false;
}

void
DxrLinuxWindow::pump(const std::function<void(DxrKey)> &on_key, bool *running)
{
	// The original key-only API, as a view of the event stream: every key
	// press (auto-repeat included, as before) that maps to a DxrKey. F11 was
	// already handled inside pump_impl().
	pump_impl(
	    [&on_key](const DxrWindowEvent &ev) {
		    if (ev.type != DxrWindowEvent::Type::KeyDown || !on_key) {
			    return;
		    }
		    const DxrKey k = dxr_key_from_keysym((KeySym)ev.keysym);
		    if (k != DxrKey::Unknown && k != DxrKey::F11) {
			    on_key(k);
		    }
	    },
	    running);
}

void
DxrLinuxWindow::pump_events(const std::function<void(const DxrWindowEvent &)> &on_event, bool *running)
{
	pump_impl(on_event, running);
}

void
DxrLinuxWindow::note_content_size()
{
	uint32_t w = 0, h = 0;
	if (!current_size(&w, &h)) {
		return;
	}
	if (w == m_reported_w && h == m_reported_h) {
		return;
	}
	const bool first = m_reported_w == 0 && m_reported_h == 0;
	m_reported_w = w;
	m_reported_h = h;
	if (first) {
		return; // the create-time size is known to the app already
	}
	DxrWindowEvent ev;
	ev.type = DxrWindowEvent::Type::Resize;
	ev.width = w;
	ev.height = h;
	m_events.push_back(ev);
}

//! X11 modifier state -> DxrKeyMod.
static uint32_t
dxr_mods_from_x11(unsigned int state)
{
	uint32_t m = 0;
	m |= (state & ShiftMask) ? DxrModShift : 0u;
	m |= (state & ControlMask) ? DxrModCtrl : 0u;
	m |= (state & Mod1Mask) ? DxrModAlt : 0u;
	m |= (state & Mod4Mask) ? DxrModSuper : 0u;
	return m;
}

void
DxrLinuxWindow::frame_stats_tick()
{
	if (m_stats_period <= 0.0) {
		return;
	}
	const int64_t now =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
	        .count();
	if (m_stats_last_ns == 0) {
		m_stats_last_ns = m_stats_window_start_ns = now;
		DXRW_INFO("frame stats: ON — every %.1f s (pump-to-pump frame time)", m_stats_period);
		return;
	}
	m_stats_ms.push_back((float)((double)(now - m_stats_last_ns) / 1e6));
	m_stats_last_ns = now;
	const double window_s = (double)(now - m_stats_window_start_ns) / 1e9;
	if (window_s < m_stats_period || m_stats_ms.empty()) {
		return;
	}
	std::vector<float> v = m_stats_ms;
	std::sort(v.begin(), v.end());
	double sum = 0.0;
	for (float x : v) {
		sum += x;
	}
	const double avg = sum / (double)v.size();
	const float p95 = v[(size_t)((double)(v.size() - 1) * 0.95)];
	uint32_t cw = 0, ch = 0;
	current_size(&cw, &ch);
	DXRW_INFO("frame stats: %zu frames in %.1f s — avg %.2f ms (%.1f fps), p95 %.2f ms, max %.2f ms; %s, "
	          "content %ux%u, %s",
	          v.size(), window_s, avg, avg > 0.0 ? 1000.0 / avg : 0.0, p95, v.back(), backend_name(m_backend),
	          cw, ch, is_fullscreen() ? "fullscreen" : "windowed");
	m_stats_ms.clear();
	m_stats_window_start_ns = now;
}

void
DxrLinuxWindow::pump_impl(const std::function<void(const DxrWindowEvent &)> &on_event, bool *running)
{
	frame_stats_tick();
#ifdef DXR_APP_HAVE_WL_CHROME
	// DXR_WL_TEST_LATTICE="dx,dy" (test hook, off by default): with nobody at
	// the mouse, do what a title-bar press does — build and send the drag
	// lattice — then move the window by (dx, dy) logical through the
	// compositor. A programmatic move is a MOVE action like a drag, so it
	// passes through the same constraint, and where it LANDS (read back from
	// the geometry service) is the test.
	if (m_backend == DxrWindowBackend::Wayland) {
		struct TestLattice
		{
			int dx = 0, dy = 0;
			bool armed = false;
		};
		static const TestLattice tl = [] {
			TestLattice c;
			const char *e = getenv("DXR_WL_TEST_LATTICE");
			if (e != nullptr && sscanf(e, "%d,%d", &c.dx, &c.dy) == 2) {
				c.armed = true;
			}
			return c;
		}();
		// DXR_WL_TEST_LATTICE_AT=N picks the pump it fires on (default 150).
		// A headless compositor may stop releasing swapchain images after a
		// frame or two, so a harness running there fires early; the test needs
		// a mapped window, not rendering.
		static const long at = [] {
			const char *e = getenv("DXR_WL_TEST_LATTICE_AT");
			const long v = e != nullptr ? strtol(e, nullptr, 10) : 150;
			return v > 0 ? v : 150;
		}();
		if (tl.armed && !m_wl_test_lattice_done && ++m_wl_test_lattice_pumps == (uint64_t)at) {
			m_wl_test_lattice_done = true;
			// The compositor places a new window asynchronously; until it has,
			// its frame reads (0,0) and a start recorded then is fiction (and
			// the placement then overrides any move). Re-send until the start
			// is real, bounded. Blocking is acceptable in a test hook only.
			for (int tries = 0; tries < 40; tries++) {
				wl_drag_prepare();
				if (!m_wl_lattice_active || m_wl_lattice_start_x != 0 || m_wl_lattice_start_y != 0) {
					break;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
			if (m_wl_lattice_active) {
				const int32_t tx = m_wl_lattice_start_x + tl.dx, ty = m_wl_lattice_start_y + tl.dy;
				const bool moved = m_wl_placement.test_move_to(tx, ty);
				DXRW_INFO("DXR_WL_TEST_LATTICE: start (%d, %d); asked the compositor for (%d, %d) = start %+d,%+d "
				          "— %s. Read the landed frame from the geometry service.",
				          m_wl_lattice_start_x, m_wl_lattice_start_y, tx, ty, tl.dx, tl.dy,
				          moved ? "accepted" : "REFUSED");
			} else {
				DXRW_WARN("DXR_WL_TEST_LATTICE: no lattice was sent — nothing to test (is DXR_WL_DRAG_LATTICE=0 set? "
				          "and check the 'drag lattice' lines above)");
			}
		}
	}
#endif

	// DXR_TEST_FULLSCREEN_TOGGLE=N (test hook, off by default): press F11 at
	// pump N and again at 2N, so the toggle is verifiable with nobody at the
	// keyboard — the same toggle_fullscreen() the key drives.
	{
		static const long s_toggle_every = [] {
			const char *e = getenv("DXR_TEST_FULLSCREEN_TOGGLE");
			return e != nullptr ? strtol(e, nullptr, 10) : 0L;
		}();
		if (s_toggle_every > 0) {
			m_test_fs_pumps++;
			if (m_test_fs_pumps == (uint64_t)s_toggle_every || m_test_fs_pumps == 2 * (uint64_t)s_toggle_every) {
				DXRW_INFO("DXR_TEST_FULLSCREEN_TOGGLE: simulated F11 at pump %llu",
				          (unsigned long long)m_test_fs_pumps);
				toggle_fullscreen();
			}
		}
	}

	if (m_backend == DxrWindowBackend::X11) {
		// Pump pending X events. The runtime borrows this Display's connection
		// for its XCB surface but never reads events from it, so the app owns
		// the queue.
		if (m_x_display == nullptr) {
			return;
		}
		m_x_pump_count++;

		// Motion coalescing (#1588): the X server can queue dozens of
		// MotionNotify per frame and acting on each would issue dozens of
		// XMoveWindow for one visible step. Only the LAST one matters — the
		// target is derived from the absolute pointer position, not from a
		// delta chain — so the whole queue is drained first and one move is
		// issued below. (App-facing Motion events are NOT coalesced: an orbit
		// is a delta chain, applied per event like WM_MOUSEMOVE.)
		bool have_motion = false;
		int motion_root_x = 0;
		int motion_root_y = 0;
		const int off_y = x11_content_offset_y();

		while (XPending(m_x_display) > 0) {
			XEvent ev;
			XNextEvent(m_x_display, &ev);
			DxrWindowEvent out;
			switch (ev.type) {
			case ClientMessage:
				if ((Atom)ev.xclient.data.l[0] == m_x_wm_delete) {
					DXRW_INFO("Window closed by user — exiting");
					if (running != nullptr) {
						*running = false;
					}
				}
				break;

			case KeyPress: {
				const unsigned int kc = ev.xkey.keycode & 0xff;
				out.type = DxrWindowEvent::Type::KeyDown;
				out.keysym = (uint32_t)XLookupKeysym(&ev.xkey, 0);
				out.keycode = kc;
				out.repeat = m_x_keys_down.test(kc);
				out.mods = dxr_mods_from_x11(ev.xkey.state);
				out.time_ms = (uint32_t)ev.xkey.time;
				m_x_keys_down.set(kc);
				if (out.keysym == XK_F11 && !out.repeat) {
					toggle_fullscreen(); // a window concern: handled here
				}
				m_events.push_back(out);
				break;
			}

			case KeyRelease: {
				const unsigned int kc = ev.xkey.keycode & 0xff;
				// Without detectable auto-repeat the server fakes a Release
				// right before each repeated Press, at the same timestamp;
				// drop it, so a held key stays held.
				if (!m_x_detectable_repeat && XEventsQueued(m_x_display, QueuedAfterReading) > 0) {
					XEvent next;
					XPeekEvent(m_x_display, &next);
					if (next.type == KeyPress && next.xkey.time == ev.xkey.time &&
					    next.xkey.keycode == ev.xkey.keycode) {
						break;
					}
				}
				m_x_keys_down.reset(kc);
				out.type = DxrWindowEvent::Type::KeyUp;
				out.keysym = (uint32_t)XLookupKeysym(&ev.xkey, 0);
				out.keycode = kc;
				out.mods = dxr_mods_from_x11(ev.xkey.state);
				out.time_ms = (uint32_t)ev.xkey.time;
				m_events.push_back(out);
				break;
			}

			case ButtonPress: {
				const unsigned int b = ev.xbutton.button;
				out.mods = dxr_mods_from_x11(ev.xbutton.state);
				out.time_ms = (uint32_t)ev.xbutton.time;
				out.x = ev.xbutton.x;
				out.y = ev.xbutton.y - off_y;
				// X11 delivers the wheel as buttons 4/5 (vertical) and 6/7.
				if (b >= Button4 && b <= 7) {
					out.type = DxrWindowEvent::Type::Scroll;
					out.scroll_steps = b == Button4 ? 1 : (b == Button5 ? -1 : 0);
					out.scroll_steps_x = b == 6 ? -1 : (b == 7 ? 1 : 0);
					m_events.push_back(out);
					break;
				}
				// Header bar first: a press there is window chrome, never a
				// click into the scene underneath it.
				if (x11_bar_visible() && ev.xbutton.y < off_y) {
					if (b == Button1) {
						const dxr_csd::Hit hit = m_x_bar.hitTest(ev.xbutton.x, ev.xbutton.y, m_x_top_w);
						if (hit == dxr_csd::Hit::Drag) {
							// The same snapped path as the drag button — the
							// reason the bar is client-side at all.
							if (m_x_client_drag && !m_x_test_drag_armed && !m_x_dragging) {
								x11_begin_drag(ev.xbutton.x_root, ev.xbutton.y_root, Button1);
								m_x_drag_from_bar = true;
							}
						} else if (hit != dxr_csd::Hit::Outside) {
							m_x_bar.setPressed(hit);
						}
					}
					break;
				}
				out.type = DxrWindowEvent::Type::ButtonDown;
				out.button = b;
				if (m_desc.x11_drag_button != 0 && b == m_desc.x11_drag_button && m_x_client_drag &&
				    !m_x_test_drag_armed && !m_x_dragging) {
					// The content drag button: the whole window is the drag
					// handle (an undecorated window has none, and a
					// transparent one may have little else).
					out.window_drag = true;
					x11_begin_drag(ev.xbutton.x_root, ev.xbutton.y_root, b);
				}
				m_events.push_back(out);
				break;
			}

			case ButtonRelease: {
				const unsigned int b = ev.xbutton.button;
				if (b >= Button4 && b <= 7) {
					break;
				}
				out.mods = dxr_mods_from_x11(ev.xbutton.state);
				out.time_ms = (uint32_t)ev.xbutton.time;
				out.x = ev.xbutton.x;
				out.y = ev.xbutton.y - off_y;
				// A drag ends on the release of the button that started it.
				if (m_x_dragging && b == m_x_drag_btn) {
					const bool from_bar = m_x_drag_from_bar;
					x11_end_drag();
					if (from_bar) {
						break; // the bar consumed this press; the app never saw it
					}
					out.window_drag = true;
				}
				// Header-bar button: fires on release over the SAME button,
				// like GTK — press, slide off, release cancels.
				if (b == Button1 && m_x_bar.pressed() != dxr_csd::Hit::Outside) {
					const dxr_csd::Hit down = m_x_bar.pressed();
					m_x_bar.setPressed(dxr_csd::Hit::Outside);
					if (x11_bar_visible() && m_x_bar.hitTest(ev.xbutton.x, ev.xbutton.y, m_x_top_w) == down) {
						if (down == dxr_csd::Hit::Close) {
							DXRW_INFO("Header bar close — exiting");
							if (running != nullptr) {
								*running = false;
							}
						} else if (down == dxr_csd::Hit::Minimize) {
							DXRW_INFO("Header bar minimize");
							XIconifyWindow(m_x_display, m_x_window, DefaultScreen(m_x_display));
							XFlush(m_x_display);
						}
					}
					break;
				}
				out.type = DxrWindowEvent::Type::ButtonUp;
				out.button = b;
				m_events.push_back(out);
				break;
			}

			case MotionNotify:
				if (m_x_dragging) {
					have_motion = true;
					motion_root_x = ev.xmotion.x_root;
					motion_root_y = ev.xmotion.y_root;
					break;
				}
				if (x11_bar_visible()) {
					m_x_bar.setHover(m_x_bar.hitTest(ev.xmotion.x, ev.xmotion.y, m_x_top_w));
				}
				out.type = DxrWindowEvent::Type::Motion;
				out.x = ev.xmotion.x;
				out.y = ev.xmotion.y - off_y;
				out.mods = dxr_mods_from_x11(ev.xmotion.state);
				out.time_ms = (uint32_t)ev.xmotion.time;
				m_events.push_back(out);
				break;

			case ConfigureNotify:
				if (ev.xconfigure.window == m_x_window && ev.xconfigure.width > 0 && ev.xconfigure.height > 0) {
					m_x_top_w = (uint32_t)ev.xconfigure.width;
					m_x_top_h = (uint32_t)ev.xconfigure.height;
					x11_layout_content();
				}
				break;

			case LeaveNotify:
				m_x_bar.setHover(dxr_csd::Hit::Outside);
				if (!m_x_dragging) {
					out.type = DxrWindowEvent::Type::PointerLeave;
					m_events.push_back(out);
				}
				break;

			case Expose: m_x_bar.invalidate(); break;

			case FocusIn:
			case FocusOut:
				// Grab/ungrab focus churn (our own drag grab) is not a real
				// activation change — ignore it, or the bar flickers per drag
				// and an app's held keys are dropped mid-drag.
				if (ev.xfocus.mode == NotifyNormal || ev.xfocus.mode == NotifyWhileGrabbed) {
					m_x_bar.setFocused(ev.type == FocusIn);
					if (ev.type == FocusOut) {
						m_x_keys_down.reset();
						// A drag whose button-up will land in another window
						// must not keep the window glued to the pointer.
						x11_end_drag();
					}
					out.type = ev.type == FocusIn ? DxrWindowEvent::Type::FocusGained
					                              : DxrWindowEvent::Type::FocusLost;
					m_events.push_back(out);
				}
				break;

			default: break;
			}
		}

		if (have_motion && m_x_dragging) {
			// Absolute, not incremental: origin + (pointer now - pointer at
			// grab). A snap that holds the window back for a few pixels
			// therefore never makes the window lag the pointer permanently.
			x11_move_snapped(m_x_drag_origin_x + (motion_root_x - m_x_drag_ptr_x),
			                 m_x_drag_origin_y + (motion_root_y - m_x_drag_ptr_y));
		}

		x11_drive_test_drag();
		x11_paint_bar();
		note_content_size();
	}

#ifdef DXR_APP_HAVE_WAYLAND
	else if (m_backend == DxrWindowBackend::Wayland) {
		if (m_wl_display == nullptr) {
			return;
		}
		// Strictly non-blocking. wl_display_dispatch() would sleep until the
		// compositor says something, stalling the render loop; and
		// wl_display_read_events() on its own blocks too. The prepare_read /
		// poll(timeout 0) / read_events dance is the documented way to drain
		// the socket without ever waiting.
		bool lost = false;
		while (wl_display_prepare_read(m_wl_display) != 0) {
			if (wl_display_dispatch_pending(m_wl_display) < 0) {
				lost = true;
				break;
			}
		}
		if (!lost) {
			wl_display_flush(m_wl_display);
			struct pollfd pfd = {};
			pfd.fd = wl_display_get_fd(m_wl_display);
			pfd.events = POLLIN;
			if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN) != 0) {
				if (wl_display_read_events(m_wl_display) < 0) {
					lost = true;
				}
			} else {
				wl_display_cancel_read(m_wl_display);
			}
		}
		if (!lost && wl_display_dispatch_pending(m_wl_display) < 0) {
			lost = true;
		}
		if (lost) {
			DXRW_ERROR("Wayland connection lost — exiting");
			if (running != nullptr) {
				*running = false;
			}
			m_events.clear();
			return;
		}
		wl_display_flush(m_wl_display);

#ifdef DXR_APP_HAVE_WL_CHROME
		if (m_wl_chrome.take_close_request()) {
			m_wl_close_requested = true;
		}
		// Hover / focus / scale changes from the dispatch above repaint the
		// bar here; a no-op when nothing it shows changed.
		m_wl_chrome.update(m_wl_config_w, m_wl_config_h, wl_surface_scale());
#endif
		if (m_wl_close_requested) {
			m_wl_close_requested = false;
			DXRW_INFO("Window closed by user — exiting");
			if (running != nullptr) {
				*running = false;
			}
		}
		m_wl_pumps++;
		if (m_wl_fs_deferred) {
			if (!m_wl_entered.empty()) {
				wl_request_panel_fullscreen("the surface is mapped");
			} else if (m_wl_pumps >= 240) {
				// Never mapped within ~4 s at 60 Hz (no session presenting?).
				// Ask anyway rather than never: the placement log below says
				// whether the compositor honoured the output.
				wl_request_panel_fullscreen("no wl_surface.enter after 240 pumps — requesting unmapped");
			}
		}
		if (m_wl_output_report_in > 0 && --m_wl_output_report_in == 0) {
			m_wl_output_report_in = -1;
			wl_report_fullscreen_output();
		}

		// A configure may have changed the size in the dispatch above. The
		// runtime has no other way to learn it — Wayland gives the WSI no
		// currentExtent — so republish before the next frame is drawn.
		publish_wayland_geometry_if_changed();
#ifdef DXR_APP_HAVE_WL_CHROME
		// The drag left the table's coverage: send the next piece, centred on
		// where the compositor says it is. Asynchronous — the compositor keeps
		// dragging (unsnapped) meanwhile and never waits on us.
		{
			wl_poll_lattice_job();
			// A drag that has not got a table yet: re-check every ~1/3 s
			// whether the window has reached the panel (its monitor changes
			// only once most of it is there, which no Wayland event reports).
			if (m_wl_compositor_drag && !m_wl_lattice_active && !m_wl_lattice_job_running &&
			    ++m_wl_lattice_retry_in >= 20) {
				m_wl_lattice_retry_in = 0;
				wl_lattice_on_placement_change();
			}
			int32_t ndx = 0, ndy = 0;
			if (m_wl_placement.poll_needed(&ndx, &ndy) && m_wl_lattice_active) {
				wl_request_lattice_async(true, ndx, ndy);
			}
			DxrWlPlacement::DragDone d;
			if (m_wl_placement.take_done(&d)) {
				// One line per drag that had a table (#1609 follow-up): what
				// the app spent, and what the compositor saw.
				const LatticeDragStats &a = m_wl_drag_stats;
				DXRW_INFO("drag lattice summary: %u table(s) + %u extension(s)%s%s, %.0f ms probing (max %.0f "
				          "ms, %u on the worker); compositor moves %u, corrected %u (%.0f%%), OFF-table %u "
				          "(%.0f%%), largest correction %u logical px; drop %s the table",
				          a.tables, a.extensions, a.entered_mid_drag ? ", first sent on reaching the panel" : "",
				          a.clears ? ", dropped on leaving it" : "", a.probe_ms, a.probe_ms_max, a.async_jobs,
				          d.moves, d.corrected, d.moves ? 100.0 * d.corrected / d.moves : 0.0, d.misses,
				          d.moves ? 100.0 * d.misses / d.moves : 0.0, d.max_correction,
				          d.landed_on_table ? "ON" : "OFF");
				m_wl_compositor_drag = false;
				m_wl_lattice_active = false;
			}
		}
#endif

		// F11 is a window concern: handled here, still delivered.
		for (const DxrWindowEvent &e : m_events) {
			if (e.type == DxrWindowEvent::Type::KeyDown && e.keysym == XK_F11) {
				toggle_fullscreen();
			}
		}
		note_content_size();
	}
#endif

	// Deliver. Swapped out first: a handler may call back into the window
	// (toggle_fullscreen, set_title, ...), which may queue more.
	std::vector<DxrWindowEvent> batch;
	batch.swap(m_events);
	if (on_event) {
		for (const DxrWindowEvent &e : batch) {
			on_event(e);
		}
	}
}

bool
DxrLinuxWindow::current_size(uint32_t *w, uint32_t *h) const
{
	if (w == nullptr || h == nullptr) {
		return false;
	}

	if (m_backend == DxrWindowBackend::X11) {
		// The BOUND window (the content child under a header bar): its size is
		// the swapchain's, never the bar-inclusive top-level's.
		XWindowAttributes wa = {};
		if (m_x_display != nullptr && m_x_window != 0 &&
		    XGetWindowAttributes(m_x_display, x11_bound_window(), &wa) && wa.width > 0 && wa.height > 0) {
			*w = (uint32_t)wa.width;
			*h = (uint32_t)wa.height;
			return true;
		}
		return false;
	}

#ifdef DXR_APP_HAVE_WAYLAND
	if (m_backend == DxrWindowBackend::Wayland) {
		// The BUFFER size — the one declared to the runtime through
		// XrWaylandSurfaceGeometryDXR, which is the swapchain the runtime
		// presents and so the space every caller's numbers live in (eye
		// tiles, zone rects). NOT the xdg_toplevel.configure size: that is
		// logical, and on a fullscreen output at a non-unit scale the declared
		// buffer is the output's MODE instead. Reading the configure there
		// (1920x1080 on a 3840x2160 panel at 200 %) rendered each eye at a
		// quarter of its tile and left the compositor to upscale it — the
		// XWayland leg, whose X11 window is already device-sized, never did.
		// Equal to the configure size at scale 1.0 and for a windowed surface.
		uint32_t dw = 0, dh = 0;
		wl_declared_size(&dw, &dh);
		if (dw > 0 && dh > 0) {
			*w = dw;
			*h = dh;
			return true;
		}
		if (m_desc.width > 0 && m_desc.height > 0) {
			*w = m_desc.width;
			*h = m_desc.height;
			return true;
		}
	}
#endif
	return false;
}

const void *
DxrLinuxWindow::session_binding_chain(const void *next)
{
	if (m_backend == DxrWindowBackend::X11 && m_x_window != 0) {
		m_xlib_binding = {};
		m_xlib_binding.type = XR_TYPE_XLIB_WINDOW_BINDING_CREATE_INFO_DXR;
		m_xlib_binding.next = next;
		m_xlib_binding.xDisplay = m_x_display;
		m_xlib_binding.window = x11_bound_window();
		m_xlib_binding.transparentBackgroundEnabled = m_transparent ? XR_TRUE : XR_FALSE;
		return &m_xlib_binding;
	}

#ifdef DXR_APP_HAVE_WAYLAND
	if (m_backend == DxrWindowBackend::Wayland && m_wl_surface != nullptr) {
		// Spec v2: declare the SIZE. A wl_surface has none of its own — the
		// WSI reports currentExtent == UINT32_MAX and the buffer the runtime
		// attaches is what defines the surface — so omitting this does not
		// leave the window alone, it lets the runtime resize it to the panel.
		// The size is the configure this helper already acked in create().
		uint32_t decl_w = 0, decl_h = 0;
		wl_declared_size(&decl_w, &decl_h);
		m_wl_geometry = {};
		m_wl_geometry.type = XR_TYPE_WAYLAND_SURFACE_GEOMETRY_DXR;
		m_wl_geometry.next = next;
		m_wl_geometry.width = decl_w;
		m_wl_geometry.height = decl_h;
		m_wl_geometry.refreshMilliHertz = m_wl_refresh_mhz;
		m_wl_published_w = decl_w;
		m_wl_published_h = decl_h;

		m_wl_binding = {};
		m_wl_binding.type = XR_TYPE_WAYLAND_SURFACE_BINDING_CREATE_INFO_DXR;
		m_wl_binding.next = &m_wl_geometry;
		m_wl_binding.wlDisplay = m_wl_display;
		m_wl_binding.wlSurface = m_wl_surface;
		m_wl_binding.transparentBackgroundEnabled = m_transparent ? XR_TRUE : XR_FALSE;
		return &m_wl_binding;
	}
#endif
	return next;
}

#ifdef DXR_APP_HAVE_WAYLAND
void
DxrLinuxWindow::wl_declared_size(uint32_t *w, uint32_t *h) const
{
	*w = 0;
	*h = 0;
#ifdef DXR_APP_HAVE_WAYLAND
	// Fullscreen on a matched output: declare the output's MODE, in device
	// pixels. That is the buffer that maps 1:1 onto the panel; the configure
	// size is logical and, on a fractionally-scaled desktop, smaller.
	if (m_wl_fullscreen_mode_w > 0 && m_wl_fullscreen_mode_h > 0) {
		*w = (uint32_t)m_wl_fullscreen_mode_w;
		*h = (uint32_t)m_wl_fullscreen_mode_h;
		return;
	}
	// Windowed (or fullscreen with no output match): the configure size (the
	// LOGICAL size the compositor will paint) times the surface's preferred
	// scale from wp_fractional_scale_v1 — the device-pixel buffer that the
	// wp_viewport destination then maps 1:1 onto that region. Before the
	// compositor has sent a preferred scale (it does once the surface enters
	// an output) this is the configure size, and the runtime's 1:1 gate keeps
	// the session flat until the scale arrives and the declaration follows.
	if (m_wl_config_w > 0 && m_wl_config_h > 0) {
		double scale = 1.0;
		// A fractional buffer size needs the viewport; an integer scale can
		// also be expressed with set_buffer_scale.
		if (m_wl_pref_scale_120 > 0 &&
		    (m_wl_viewport != nullptr || m_wl_pref_scale_120 % 120 == 0)) {
			scale = (double)m_wl_pref_scale_120 / 120.0;
		}
		*w = (uint32_t)((double)m_wl_config_w * scale + 0.5);
		*h = (uint32_t)((double)m_wl_config_h * scale + 0.5);
	}
#endif
}

void
DxrLinuxWindow::wl_update_opaque_region()
{
#ifdef DXR_APP_HAVE_WAYLAND
	if (m_wl_surface == nullptr || m_wl_compositor == nullptr) {
		return;
	}
	const int32_t w = m_transparent_bg ? 0 : m_wl_config_w;
	const int32_t h = m_transparent_bg ? 0 : m_wl_config_h;
	if (w == m_wl_opaque_w && h == m_wl_opaque_h) {
		return;
	}
	if (w <= 0 || h <= 0) {
		// Transparent background (or no size yet): no opaque region, so the
		// compositor blends the surface over the desktop, as it must.
		wl_surface_set_opaque_region(m_wl_surface, nullptr);
	} else {
		// Surface-local LOGICAL coordinates: the viewport destination, i.e.
		// exactly the configured rect the buffer is mapped onto.
		struct wl_region *region = wl_compositor_create_region(m_wl_compositor);
		wl_region_add(region, 0, 0, w, h);
		wl_surface_set_opaque_region(m_wl_surface, region);
		wl_region_destroy(region);
	}
	if (m_wl_opaque_w < 0) {
		DXRW_INFO("Wayland: opaque region %s", w > 0 ? "= the whole content surface (opaque window)"
		                                             : "none (transparent background)");
	}
	m_wl_opaque_w = w;
	m_wl_opaque_h = h;
#endif
}

void
DxrLinuxWindow::wl_apply_buffer_mapping()
{
#ifdef DXR_APP_HAVE_WAYLAND
	if (m_wl_surface == nullptr || m_wl_config_w <= 0 || m_wl_config_h <= 0) {
		return;
	}
	wl_update_opaque_region(); // follows every configured size (runtime#1698)
	uint32_t bw = 0, bh = 0;
	wl_declared_size(&bw, &bh);
	if (bw == 0 || bh == 0) {
		return;
	}
	if (m_wl_viewport != nullptr) {
		// Destination = the configure size, whatever the buffer is: the
		// compositor scales the whole buffer onto exactly the configured
		// region, which is 1:1 whenever the buffer is logical x scale.
		if (m_wl_config_w == m_wl_map_dst_w && m_wl_config_h == m_wl_map_dst_h) {
			return;
		}
		wp_viewport_set_destination(m_wl_viewport, m_wl_config_w, m_wl_config_h);
		m_wl_map_dst_w = m_wl_config_w;
		m_wl_map_dst_h = m_wl_config_h;
		DXRW_INFO("Wayland: %ux%u buffer -> wp_viewport destination %dx%d logical", bw, bh, m_wl_config_w,
		          m_wl_config_h);
		return;
	}
	// No viewporter: only an exact integer ratio can be expressed.
	int32_t scale = 1;
	if (bw % (uint32_t)m_wl_config_w == 0 && bh % (uint32_t)m_wl_config_h == 0 &&
	    bw / (uint32_t)m_wl_config_w == bh / (uint32_t)m_wl_config_h) {
		scale = (int32_t)(bw / (uint32_t)m_wl_config_w);
	} else {
		DXRW_WARN("Wayland: %ux%u buffer for a %dx%d logical surface is not an integer scale and "
		          "wp_viewporter is absent — the compositor cannot map it 1:1; the surface will be "
		          "the wrong size and the runtime will present flat 2D.",
		          bw, bh, m_wl_config_w, m_wl_config_h);
	}
	if (scale == m_wl_map_buffer_scale) {
		return;
	}
	wl_surface_set_buffer_scale(m_wl_surface, scale);
	m_wl_map_buffer_scale = scale;
	DXRW_INFO("Wayland: %ux%u buffer -> wl_surface.set_buffer_scale(%d) for %dx%d logical", bw, bh, scale,
	          m_wl_config_w, m_wl_config_h);
#endif
}
#endif // DXR_APP_HAVE_WAYLAND

void
DxrLinuxWindow::attach_session(XrInstance instance, XrSession session)
{
	m_session = session;

#ifdef DXR_APP_HAVE_WAYLAND
	if (m_backend != DxrWindowBackend::Wayland || instance == XR_NULL_HANDLE) {
		return;
	}
	// Resolved, not linked: an older runtime (extension spec v1) has no such
	// function and xrGetInstanceProcAddr answers XR_ERROR_FUNCTION_UNSUPPORTED.
	// That is a degraded session, not a broken one — the create-time size still
	// applies, only later resizes stop being followed. Log once.
	PFN_xrVoidFunction fn = nullptr;
	if (xrGetInstanceProcAddr(instance, "xrSetWaylandSurfaceGeometryDXR", &fn) == XR_SUCCESS && fn != nullptr) {
		m_pfn_set_wl_geometry = reinterpret_cast<PFN_xrSetWaylandSurfaceGeometryDXR>(fn);
		DXRW_INFO("Wayland: xrSetWaylandSurfaceGeometryDXR resolved — surface resizes will be followed");
	} else {
		DXRW_WARN("Wayland: this runtime has no xrSetWaylandSurfaceGeometryDXR "
		          "(XR_DXR_wayland_surface_binding spec < 2). The size declared at session create still "
		          "applies; later xdg_toplevel.configure resizes will NOT be followed.");
	}
#else
	(void)instance;
#endif
}

void
DxrLinuxWindow::publish_wayland_geometry_if_changed()
{
#ifdef DXR_APP_HAVE_WAYLAND
	if (m_pfn_set_wl_geometry == nullptr || m_session == XR_NULL_HANDLE) {
		return;
	}
	uint32_t w = 0, h = 0;
	wl_declared_size(&w, &h);
	if (w == 0 || h == 0) {
		return;
	}
	if (w == m_wl_published_w && h == m_wl_published_h) {
		return;
	}
	const XrResult res = m_pfn_set_wl_geometry(m_session, w, h, m_wl_refresh_mhz);
	if (res != XR_SUCCESS) {
		DXRW_WARN("xrSetWaylandSurfaceGeometryDXR(%ux%u) failed: %d", w, h, (int)res);
		return;
	}
	m_wl_published_w = w;
	m_wl_published_h = h;
	DXRW_INFO("Wayland: declared new surface geometry %ux%u @ %u mHz", w, h, m_wl_refresh_mhz);
#endif
}

bool
DxrLinuxWindow::force_declare_geometry(uint32_t width, uint32_t height)
{
#ifdef DXR_APP_HAVE_WAYLAND
	if (m_backend != DxrWindowBackend::Wayland || width == 0 || height == 0) {
		return false;
	}
	// Pretend a configure arrived. On Wayland this is not a lie: the buffer
	// the runtime attaches defines the surface, so declaring a new size IS how
	// a client resizes itself. Drop any fullscreen mode override — the test
	// hook is asking for this exact buffer size, not the output's.
	m_wl_fullscreen_mode_w = 0;
	m_wl_fullscreen_mode_h = 0;
	m_wl_config_w = (int32_t)width;
	m_wl_config_h = (int32_t)height;
	publish_wayland_geometry_if_changed();
	return m_wl_published_w == width && m_wl_published_h == height;
#else
	(void)width;
	(void)height;
	return false;
#endif
}

bool
DxrLinuxWindow::toggle_fullscreen()
{
	if (m_backend == DxrWindowBackend::X11 && m_x_display != nullptr && m_x_window != 0) {
		// Same EWMH recipe create_x11() uses for a panel-sized window: the
		// fullscreen state, pinned to the panel's RandR monitor.
		const bool want = !m_x_fullscreen;
		Atom net_wm_state = XInternAtom(m_x_display, "_NET_WM_STATE", False);
		Atom net_wm_state_fullscreen = XInternAtom(m_x_display, "_NET_WM_STATE_FULLSCREEN", False);
		if (net_wm_state == None || net_wm_state_fullscreen == None) {
			return false;
		}
		if (want) {
			const X11MonitorRect mon = x11_resolve_monitor(m_x_display, DefaultRootWindow(m_x_display),
			                                               m_desc.panel_left, m_desc.panel_top);
			if (mon.index >= 0) {
				Atom net_fs_monitors = XInternAtom(m_x_display, "_NET_WM_FULLSCREEN_MONITORS", False);
				if (net_fs_monitors != None) {
					x11_send_root_message(m_x_display, m_x_window, net_fs_monitors, mon.index,
					                      mon.index, mon.index, mon.index, 1 /* source: application */);
				}
			}
		}
		x11_send_root_message(m_x_display, m_x_window, net_wm_state, want ? 1 /* ADD */ : 0 /* REMOVE */,
		                      (long)net_wm_state_fullscreen, 0, 1 /* source: application */, 0);
		XFlush(m_x_display);
		m_x_fullscreen = want;
		// A fullscreen window must not be draggable (a stray click would slide
		// the panel-sized weave off the panel); a windowed one owns its drag
		// unless the WM does.
		m_x_client_drag = !want && !m_x_wm_drag;
		if (want && m_x_dragging) {
			x11_end_drag();
		}
		// The bar hides / returns now; the sizes settle on the ConfigureNotify
		// that follows, which lays the content out again.
		x11_layout_content();
		DXRW_INFO("F11: X11 window %s", want ? "fullscreen on the 3D panel's monitor" : "windowed");
		return true;
	}
#ifdef DXR_APP_HAVE_WAYLAND
	if (m_backend == DxrWindowBackend::Wayland && m_wl_toplevel != nullptr) {
		if (m_wl_fs_deferred) {
			// F11 before the deferred request went out: the user wants the
			// window, not the panel — drop the pending fullscreen.
			m_wl_fs_deferred = false;
			m_wl_fullscreen_mode_w = 0;
			m_wl_fullscreen_mode_h = 0;
			DXRW_INFO("F11: pending fullscreen cancelled — staying windowed");
		} else if (m_wl_fullscreen) {
			xdg_toplevel_unset_fullscreen(m_wl_toplevel);
			m_wl_unfullscreen_pending = true;
			DXRW_INFO("F11: leaving fullscreen");
		} else {
			// Onto the 3D panel's output when one matched (a 3D app belongs
			// there, and it is the INV-1.3 start-up placement too); else the
			// compositor's choice — the output the window is on.
			m_wl_fs_on_panel = m_wl_panel_output != nullptr;
			xdg_toplevel_set_fullscreen(m_wl_toplevel, m_wl_panel_output);
			m_wl_output_report_in = 30; // judge where it lands
			m_wl_output_reported.clear();
			DXRW_INFO("F11: fullscreen on %s", m_wl_fs_on_panel ? "the 3D panel's wl_output"
			                                                   : "the compositor's choice (no panel output matched)");
		}
		wl_display_flush(m_wl_display);
		return true;
	}
#endif
	return false;
}

bool
DxrLinuxWindow::header_bar_visible() const
{
	if (m_backend == DxrWindowBackend::X11) {
		return x11_bar_visible();
	}
#if defined(DXR_APP_HAVE_WAYLAND) && defined(DXR_APP_HAVE_WL_CHROME)
	if (m_backend == DxrWindowBackend::Wayland) {
		return !m_wl_fullscreen && m_wl_chrome.bar_logical() > 0;
	}
#endif
	return false;
}

bool
DxrLinuxWindow::is_fullscreen() const
{
	if (m_backend == DxrWindowBackend::X11) {
		return m_x_fullscreen;
	}
#ifdef DXR_APP_HAVE_WAYLAND
	return m_wl_fullscreen;
#else
	return false;
#endif
}

void
DxrLinuxWindow::set_snap_provider(SnapWindowOriginFn fn, void *userdata)
{
	m_snap_fn = fn;
	m_snap_userdata = userdata;
}

const char *
DxrLinuxWindow::required_openxr_extension() const
{
	switch (m_backend) {
	case DxrWindowBackend::X11: return XR_DXR_XLIB_WINDOW_BINDING_EXTENSION_NAME;
	case DxrWindowBackend::Wayland: return XR_DXR_WAYLAND_SURFACE_BINDING_EXTENSION_NAME;
	default: return nullptr;
	}
}

std::string
DxrLinuxWindow::describe() const
{
	char buf[192];
	if (m_backend == DxrWindowBackend::X11) {
		if (m_x_content != 0) {
			snprintf(buf, sizeof(buf), "X11 Display %p, Window 0x%lx (content child of 0x%lx)",
			         (void *)m_x_display, m_x_content, m_x_window);
		} else {
			snprintf(buf, sizeof(buf), "X11 Display %p, Window 0x%lx", (void *)m_x_display, m_x_window);
		}
		return buf;
	}
#ifdef DXR_APP_HAVE_WAYLAND
	if (m_backend == DxrWindowBackend::Wayland) {
		snprintf(buf, sizeof(buf), "wl_display %p, wl_surface %p", (void *)m_wl_display, (void *)m_wl_surface);
		return buf;
	}
#endif
	return "no window";
}

void
DxrLinuxWindow::verify_connection(DxrWindowBackend requested)
{
	// GLFW-style: report the platform we GOT, read off the live connection,
	// not the one that was asked for or the macros this was compiled with.
	char buf[224];
	DxrWindowBackend got = DxrWindowBackend::Auto;
	if (m_x_display != nullptr && m_x_window != 0) {
		got = DxrWindowBackend::X11;
		int op = 0, ev = 0, err = 0;
		const bool xwayland = XQueryExtension(m_x_display, "XWAYLAND", &op, &ev, &err) != 0;
		snprintf(buf, sizeof(buf), "X11 (%s, %s %d, %s)", xwayland ? "XWayland" : "native X server",
		         ServerVendor(m_x_display), VendorRelease(m_x_display), DisplayString(m_x_display));
	}
#ifdef DXR_APP_HAVE_WAYLAND
	else if (m_wl_display != nullptr && m_wl_surface != nullptr) {
		got = DxrWindowBackend::Wayland;
		snprintf(buf, sizeof(buf), "Wayland (native; wp_fractional_scale_v1 %s, wp_viewporter %s)",
		         m_wl_frac_manager != nullptr ? "yes" : "no", m_wl_viewporter != nullptr ? "yes" : "no");
	}
#endif
	else {
		snprintf(buf, sizeof(buf), "none");
	}
	m_connection_desc = buf;
	{
		uint32_t cw = 0, ch = 0;
		current_size(&cw, &ch);
		char sz[48];
		snprintf(sz, sizeof(sz), "; content buffer %ux%u device px", cw, ch);
		m_connection_desc += sz;
	}
	if (got != requested) {
		DXRW_WARN("Window platform: asked for %s but the live connection is %s", backend_name(requested),
		          m_connection_desc.c_str());
	} else {
		DXRW_INFO("Window platform: %s — verified on the live connection; binding %s", m_connection_desc.c_str(),
		          required_openxr_extension());
	}
	m_backend = got;
}

void
DxrLinuxWindow::set_title(const char *title)
{
	if (title == nullptr) {
		return;
	}
	if (m_backend == DxrWindowBackend::X11 && m_x_display != nullptr && m_x_window != 0) {
		XStoreName(m_x_display, m_x_window, title);
		m_x_bar.setTitle(title); // repainted on the next pump when it changed
		XFlush(m_x_display);
		return;
	}
#ifdef DXR_APP_HAVE_WAYLAND
	if (m_backend == DxrWindowBackend::Wayland && m_wl_toplevel != nullptr) {
		xdg_toplevel_set_title(m_wl_toplevel, title);
#ifdef DXR_APP_HAVE_WL_CHROME
		m_wl_chrome.set_title(title);
#endif
		wl_display_flush(m_wl_display);
	}
#endif
}

bool
DxrLinuxWindow::set_input_region(const DxrWindowRect *rects, size_t count)
{
	if (m_backend == DxrWindowBackend::X11 && m_x_display != nullptr && m_x_window != 0) {
		if (!x11_shape_available()) {
			return false;
		}
#ifdef DXR_LW_HAVE_XSHAPE
		// The TOP-LEVEL carries the shape: it is the window the WM and
		// XWayland hit-test, and the server never descends into a child at a
		// point outside the parent's input shape, so shaping the parent alone
		// gates the content child too. Content rects shift down by the bar,
		// and the bar itself is always unioned in — an un-unioned band is
		// invisible to the pointer.
		const int off = x11_content_offset_y();
		std::vector<XRectangle> xr;
		xr.reserve(count + 1);
		for (size_t i = 0; i < count; i++) {
			if (rects[i].width == 0 || rects[i].height == 0) {
				continue;
			}
			XRectangle r;
			r.x = (short)rects[i].x;
			r.y = (short)(rects[i].y + off);
			r.width = (unsigned short)rects[i].width;
			r.height = (unsigned short)rects[i].height;
			xr.push_back(r);
		}
		if (x11_bar_visible()) {
			xr.push_back(dxr_x11_chrome::BarRect(m_x_bar, m_x_top_w));
		}
		XShapeCombineRectangles(m_x_display, m_x_window, ShapeInput, 0, 0, xr.empty() ? nullptr : xr.data(),
		                        (int)xr.size(), ShapeSet, Unsorted);
		XFlush(m_x_display);
		return true;
#endif
	}
#ifdef DXR_APP_HAVE_WAYLAND
	if (m_backend == DxrWindowBackend::Wayland && m_wl_surface != nullptr && m_wl_compositor != nullptr) {
		// Surface-local LOGICAL px, rounded OUTWARD (an under-large region
		// makes content unreachable; an over-large one leaves a pixel or two
		// clickable). Pending state on the bound surface: the runtime's next
		// present commits it, exactly like the viewport mapping. The title
		// bar is its own subsurface and keeps its full input region.
		uint32_t dw = 0, dh = 0;
		wl_declared_size(&dw, &dh);
		const double sx = (m_wl_config_w > 0 && dw > 0) ? (double)m_wl_config_w / (double)dw : 1.0;
		const double sy = (m_wl_config_h > 0 && dh > 0) ? (double)m_wl_config_h / (double)dh : 1.0;
		struct wl_region *region = wl_compositor_create_region(m_wl_compositor);
		for (size_t i = 0; i < count; i++) {
			if (rects[i].width == 0 || rects[i].height == 0) {
				continue;
			}
			const int32_t x0 = (int32_t)std::floor((double)rects[i].x * sx);
			const int32_t y0 = (int32_t)std::floor((double)rects[i].y * sy);
			const int32_t x1 = (int32_t)std::ceil((double)(rects[i].x + (int32_t)rects[i].width) * sx);
			const int32_t y1 = (int32_t)std::ceil((double)(rects[i].y + (int32_t)rects[i].height) * sy);
			wl_region_add(region, x0, y0, x1 - x0, y1 - y0);
		}
		wl_surface_set_input_region(m_wl_surface, region);
		wl_region_destroy(region);
		wl_display_flush(m_wl_display);
		return true;
	}
#endif
	(void)rects;
	(void)count;
	return false;
}

void
DxrLinuxWindow::clear_input_region()
{
	if (m_backend == DxrWindowBackend::X11 && m_x_display != nullptr && m_x_window != 0) {
#ifdef DXR_LW_HAVE_XSHAPE
		if (m_x_shape_state > 0) {
			XShapeCombineMask(m_x_display, m_x_window, ShapeInput, 0, 0, None, ShapeSet);
			XFlush(m_x_display);
		}
#endif
		return;
	}
#ifdef DXR_APP_HAVE_WAYLAND
	if (m_backend == DxrWindowBackend::Wayland && m_wl_surface != nullptr) {
		wl_surface_set_input_region(m_wl_surface, nullptr); // NULL = infinite: the whole surface
		wl_display_flush(m_wl_display);
	}
#endif
}

void
DxrLinuxWindow::set_keep_above(bool above)
{
	if (m_backend == DxrWindowBackend::X11 && m_x_display != nullptr && m_x_window != 0) {
		if (above == m_x_keep_above) {
			return;
		}
		Atom net_wm_state = XInternAtom(m_x_display, "_NET_WM_STATE", False);
		Atom above_atom = XInternAtom(m_x_display, "_NET_WM_STATE_ABOVE", False);
		if (net_wm_state == None || above_atom == None) {
			return;
		}
		x11_send_root_message(m_x_display, m_x_window, net_wm_state, above ? 1 /* ADD */ : 0 /* REMOVE */,
		                      (long)above_atom, 0, 1 /* source: application */, 0);
		XFlush(m_x_display);
		m_x_keep_above = above;
		return;
	}
	if (m_backend == DxrWindowBackend::Wayland && !m_warned_keep_above) {
		m_warned_keep_above = true;
		DXRW_INFO("Wayland: keep-above requested — no protocol for it on Wayland; ignored");
	}
}

void
DxrLinuxWindow::set_transparent_background(bool transparent)
{
	if (!m_transparent) {
		return; // not transparent-capable: nothing is ever drawn see-through
	}
	if (transparent == m_transparent_bg) {
		return;
	}
	if (m_backend == DxrWindowBackend::X11 && m_x_display != nullptr && m_x_window != 0) {
		const bool was_visible = x11_bar_visible();
		m_transparent_bg = transparent;
		const bool now_visible = x11_bar_visible();
		if (was_visible != now_visible) {
			// Keep the CONTENT where it is: the top-level gains / loses the
			// bar above it, so it moves by exactly the bar's height and
			// resizes by it. The content child's root origin and size — the
			// runtime's window rect — do not change.
			const int bar = (int)m_x_bar.height();
			int top_x = 0, top_y = 0;
			x11_root_origin(m_x_display, m_x_window, &top_x, &top_y);
			const int dy = now_visible ? -bar : bar;
			const uint32_t new_h = now_visible ? m_x_content_h + (uint32_t)bar : m_x_content_h;
			XMoveResizeWindow(m_x_display, m_x_window, top_x, top_y + dy, m_x_top_w, new_h);
			m_x_top_h = new_h;
			x11_layout_content();
			m_x_bar.invalidate();
			XFlush(m_x_display);
		}
		DXRW_INFO("X11: transparent background %s — header bar %s", transparent ? "ON" : "OFF",
		          now_visible ? "shown" : (m_x_bar_enabled ? "hidden (content rect unchanged)" : "n/a (none)"));
		return;
	}
#if defined(DXR_APP_HAVE_WAYLAND) && defined(DXR_APP_HAVE_WL_CHROME)
	if (m_backend == DxrWindowBackend::Wayland && m_wl_surface != nullptr) {
		m_transparent_bg = transparent;
		wl_update_opaque_region(); // a see-through surface must not claim to be opaque
		m_wl_chrome.set_suppressed(transparent);
		m_wl_chrome.update(m_wl_config_w, m_wl_config_h, wl_surface_scale());
		wl_display_flush(m_wl_display);
		DXRW_INFO("Wayland: transparent background %s — title bar %s", transparent ? "ON" : "OFF",
		          transparent ? "hidden (content surface and declared size unchanged)" : "restored");
		return;
	}
#endif
	m_transparent_bg = transparent;
#ifdef DXR_APP_HAVE_WAYLAND
	wl_update_opaque_region(); // no-op off Wayland
#endif
}

void
DxrLinuxWindow::set_decorated(bool decorated)
{
	if (m_backend == DxrWindowBackend::X11 && m_x_display != nullptr && m_x_window != 0) {
		if (m_x_fullscreen || decorated == m_x_wm_drag) {
			return;
		}
		x11_end_drag(); // a WM frame now owns the move (or the client does again)
		Atom motif = XInternAtom(m_x_display, "_MOTIF_WM_HINTS", False);
		if (motif != None) {
			// flags=2 MWM_HINTS_DECORATIONS; decorations 1 = all, 0 = none.
			unsigned long hints[5] = {2, 0, decorated ? 1UL : 0UL, 0, 0};
			XChangeProperty(m_x_display, m_x_window, motif, motif, 32, PropModeReplace,
			                (const unsigned char *)hints, 5);
		}
		m_x_wm_drag = decorated;
		m_x_client_drag = !decorated;
		x11_layout_content(); // the header bar (if any) hides under a WM frame
		XFlush(m_x_display);
		DXRW_INFO("X11: window decoration %s", decorated ? "ON — WM frame, WM-owned (unsnapped) move/resize"
		                                               : "OFF — borderless, client-owned snapped drag");
		return;
	}
#if defined(DXR_APP_HAVE_WAYLAND) && defined(DXR_APP_HAVE_WL_CHROME)
	if (m_backend == DxrWindowBackend::Wayland && m_wl_surface != nullptr) {
		if (m_wl_fullscreen || decorated == !m_wl_chrome.hidden()) {
			return;
		}
		m_wl_chrome.set_hidden(!decorated);
		m_wl_chrome.update(m_wl_config_w, m_wl_config_h, wl_surface_scale());
		wl_display_flush(m_wl_display);
		DXRW_INFO("Wayland: title bar %s", decorated ? "shown" : "hidden (undecorated)");
	}
#endif
}

bool
DxrLinuxWindow::is_decorated() const
{
	if (m_backend == DxrWindowBackend::X11) {
		return m_x_wm_drag;
	}
#if defined(DXR_APP_HAVE_WAYLAND) && defined(DXR_APP_HAVE_WL_CHROME)
	if (m_backend == DxrWindowBackend::Wayland) {
		return !m_wl_chrome.hidden();
	}
#endif
	return false;
}

void
DxrLinuxWindow::destroy()
{
	destroy_x11();
#ifdef DXR_APP_HAVE_WAYLAND
	destroy_wayland();
#endif
	m_backend = DxrWindowBackend::Auto;
	m_events.clear();
	m_connection_desc.clear();
}

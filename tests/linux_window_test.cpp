// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Contract test for displayxr::linux_window (common/linux/).
 *
 * Always: the --platform parser, and that the capability probe + select()
 * never pick a backend the box cannot connect to.
 *
 * With DXR_LW_TEST_X11=1 (CI runs it under Xvfb; a dev box would get real
 * windows, hence opt-in): select(auto) picks X11 when an X server answers,
 * then two real windows go through create -> pump -> destroy — the plain
 * window of the runtime's cube apps and the demos' shape (header bar, ARGB
 * visual, right-button drag) — including synthetic key and button events,
 * whose CONTENT coordinates must come back with the bar subtracted.
 */

#include "dxr_linux_window.h"

#include <X11/keysym.h>

#include <sys/mman.h> // memfd_create (the fake WSI's buffer)
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// attach_session() / DxrWeaveSnap resolve through the loader; this test links
// none, so it supplies the one entry point the library references.
extern "C" XRAPI_ATTR XrResult XRAPI_CALL
xrGetInstanceProcAddr(XrInstance, const char *, PFN_xrVoidFunction *function)
{
	if (function != nullptr) {
		*function = nullptr;
	}
	return XR_ERROR_FUNCTION_UNSUPPORTED;
}

static int g_failures = 0;

#define CHECK(cond, msg)                                                                                               \
	do {                                                                                                           \
		if (!(cond)) {                                                                                         \
			std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                          \
			g_failures++;                                                                                  \
		}                                                                                                      \
	} while (0)

static void
test_parse()
{
	auto parse = [](std::vector<const char *> args, DxrWindowBackend *out, std::string *err) {
		args.insert(args.begin(), "app");
		return DxrLinuxWindow::parse_platform_args((int)args.size(), const_cast<char **>(args.data()), out,
		                                           err);
	};
	DxrWindowBackend b = DxrWindowBackend::Auto;
	std::string err;
	CHECK(parse({"--platform=x11"}, &b, &err) && b == DxrWindowBackend::X11, "--platform=x11");
	CHECK(parse({"--platform", "wayland"}, &b, &err) && b == DxrWindowBackend::Wayland, "--platform wayland");
	CHECK(parse({"--backend=auto"}, &b, &err) && b == DxrWindowBackend::Auto, "--backend=auto (legacy spelling)");
	b = DxrWindowBackend::X11;
	CHECK(parse({"--other", "file.glb"}, &b, &err) && b == DxrWindowBackend::X11, "no flag leaves out untouched");
	CHECK(parse({"--platform=wayland", "--platform=x11"}, &b, &err) && b == DxrWindowBackend::X11, "last wins");
	CHECK(!parse({"--platform=gdi"}, &b, &err) && !err.empty(), "bad value rejected");
	CHECK(!parse({"--platform"}, &b, &err), "missing value rejected");
}

static void
test_select(const DxrWindowProbe &p)
{
	std::string why;
	const DxrWindowBackend a = DxrLinuxWindow::select(DxrWindowBackend::Auto, true, true, &why);
	std::printf("select(auto): %s — %s\n", DxrLinuxWindow::backend_name(a), why.c_str());
	if (a == DxrWindowBackend::X11) {
		CHECK(p.x11_connects, "auto never picks X11 without an X server");
	}
	if (a == DxrWindowBackend::Wayland) {
		CHECK(p.wayland_connects, "auto never picks Wayland without a compositor");
	}
	// The policy today: X11 whenever it connects.
	if (p.x11_connects) {
		CHECK(a == DxrWindowBackend::X11, "auto prefers X11 when an X server answers");
	}
	// A runtime without the xlib binding is never handed an X11 window.
	const DxrWindowBackend nx = DxrLinuxWindow::select(DxrWindowBackend::Auto, false, true, &why);
	CHECK(nx != DxrWindowBackend::X11, "auto respects the runtime's extension list");
	// Explicit requests win (even when the connection will fail).
	CHECK(DxrLinuxWindow::select(DxrWindowBackend::X11, true, true, &why) == DxrWindowBackend::X11,
	      "explicit x11 wins");
	CHECK(DxrLinuxWindow::select(DxrWindowBackend::X11, false, true, &why) == DxrWindowBackend::Auto,
	      "explicit x11 without the runtime extension is refused");
}

static void
send_key(Display *dpy, ::Window w, KeySym sym, bool press)
{
	XEvent ev = {};
	ev.xkey.type = press ? KeyPress : KeyRelease;
	ev.xkey.display = dpy;
	ev.xkey.window = w;
	ev.xkey.root = DefaultRootWindow(dpy);
	ev.xkey.keycode = XKeysymToKeycode(dpy, sym);
	ev.xkey.time = press ? 1000 : 1100;
	ev.xkey.same_screen = True;
	XSendEvent(dpy, w, False, press ? KeyPressMask : KeyReleaseMask, &ev);
}

static void
send_button(Display *dpy, ::Window w, unsigned int button, int x, int y)
{
	XEvent ev = {};
	ev.xbutton.type = ButtonPress;
	ev.xbutton.display = dpy;
	ev.xbutton.window = w;
	ev.xbutton.root = DefaultRootWindow(dpy);
	ev.xbutton.button = button;
	ev.xbutton.x = x;
	ev.xbutton.y = y;
	ev.xbutton.time = 2000;
	ev.xbutton.same_screen = True;
	XSendEvent(dpy, w, False, ButtonPressMask, &ev);
	XFlush(dpy);
}

static std::vector<DxrWindowEvent>
pump_for(DxrLinuxWindow &win, int pumps)
{
	std::vector<DxrWindowEvent> got;
	bool running = true;
	for (int i = 0; i < pumps; i++) {
		XSync(win.x11_display(), False);
		win.pump_events([&got](const DxrWindowEvent &e) { got.push_back(e); }, &running);
	}
	CHECK(running, "no close request during the test");
	return got;
}

static void
test_x11_window(bool demo_shape)
{
	DxrLinuxWindowDesc desc;
	desc.width = 640;
	desc.height = 360;
	desc.panel_left = 0;
	desc.panel_top = 0;
	desc.panel_width = 1280; // windowed: smaller than the "panel"
	desc.panel_height = 720;
	desc.title = demo_shape ? "linux_window_test (demo shape)" : "linux_window_test (plain)";
	if (demo_shape) {
		desc.transparent = true;
		desc.x11_header_bar = true;
		desc.x11_drag_button = 3;
		desc.wayland_drag_button = 3;
	}

	DxrLinuxWindow win;
	CHECK(win.create(DxrWindowBackend::X11, desc), "create X11 window");
	if (win.backend() != DxrWindowBackend::X11) {
		return;
	}
	CHECK(win.connection_description().rfind("X11", 0) == 0, "connection verified as X11");
	CHECK(std::strcmp(win.required_openxr_extension(), XR_DXR_XLIB_WINDOW_BINDING_EXTENSION_NAME) == 0,
	      "xlib binding required");

	uint32_t w = 0, h = 0;
	CHECK(win.current_size(&w, &h) && w == 640 && h == 360, "content size is the requested size (bar excluded)");

	const auto *bind = static_cast<const XrXlibWindowBindingCreateInfoDXR *>(win.session_binding_chain(nullptr));
	CHECK(bind != nullptr && bind->type == XR_TYPE_XLIB_WINDOW_BINDING_CREATE_INFO_DXR, "xlib binding struct");
	if (bind != nullptr) {
		CHECK(bind->window == win.x11_bound_window(), "the bound window is what the binding carries");
		CHECK((bind->transparentBackgroundEnabled == XR_TRUE) == win.is_transparent(),
		      "binding transparency == is_transparent()");
	}

	Display *dpy = win.x11_display();
	::Window top = 0;
	{
		// The top-level is the bound window's parent under a header bar.
		::Window root = 0, parent = 0, *kids = nullptr;
		unsigned int n = 0;
		XQueryTree(dpy, win.x11_bound_window(), &root, &parent, &kids, &n);
		if (kids != nullptr) {
			XFree(kids);
		}
		top = demo_shape ? parent : win.x11_bound_window();
	}

	(void)pump_for(win, 5); // settle

	send_key(dpy, top, XK_w, true);
	send_key(dpy, top, XK_w, false);
	// The bar's height, read off the server: top-level height - content height.
	int bar = 0;
	if (demo_shape) {
		XWindowAttributes ta = {}, ca = {};
		XGetWindowAttributes(dpy, top, &ta);
		XGetWindowAttributes(dpy, win.x11_bound_window(), &ca);
		bar = ta.height - ca.height;
		CHECK(bar > 0, "a header bar sits above the content");
	}
	send_button(dpy, top, Button1, 100, 200 + bar);
	send_button(dpy, top, Button4, 100, 200 + bar);
	auto got = pump_for(win, 5);

	bool saw_down = false, saw_up = false, saw_btn = false, saw_scroll = false;
	for (const auto &e : got) {
		if (e.type == DxrWindowEvent::Type::KeyDown && e.keysym == XK_w) {
			saw_down = true;
		}
		if (e.type == DxrWindowEvent::Type::KeyUp && e.keysym == XK_w) {
			saw_up = true;
		}
		if (e.type == DxrWindowEvent::Type::ButtonDown && e.button == 1) {
			saw_btn = true;
			CHECK(e.x == 100, "button x is content-relative");
			CHECK(e.y == 200, "button y is content-relative (the bar subtracted)");
		}
		if (e.type == DxrWindowEvent::Type::Scroll) {
			saw_scroll = e.scroll_steps == 1;
		}
	}
	CHECK(saw_down && saw_up, "key press + release delivered with keysyms");
	CHECK(saw_btn, "button press delivered");
	CHECK(saw_scroll, "wheel delivered as a Scroll step");

	const DxrWindowRect r = {10, 10, 100, 50};
	const bool shaped = win.set_input_region(&r, 1);
	std::printf("set_input_region: %s\n", shaped ? "applied" : "unavailable");
	win.clear_input_region();
	win.set_title("retitled");
	win.set_keep_above(true);
	win.set_keep_above(false);
	(void)pump_for(win, 3);

	CHECK(win.toggle_fullscreen() && win.is_fullscreen(), "F11 -> fullscreen");
	(void)pump_for(win, 3);
	CHECK(win.toggle_fullscreen() && !win.is_fullscreen(), "F11 -> windowed");
	(void)pump_for(win, 3);

	win.destroy();
	CHECK(win.backend() == DxrWindowBackend::Auto, "destroyed");
}


#ifdef DXR_APP_HAVE_WAYLAND
// A minimal stand-in for the runtime's WSI: one wl_shm buffer, attached and
// committed every pump (the helper itself never commits after create()).
struct FakeWsi
{
	struct wl_shm *shm = nullptr;
	struct wl_buffer *buffer = nullptr;
	uint32_t w = 0, h = 0;

	static void
	global(void *data, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version)
	{
		(void)version;
		if (std::strcmp(iface, "wl_shm") == 0) {
			static_cast<FakeWsi *>(data)->shm =
			    static_cast<struct wl_shm *>(wl_registry_bind(r, name, &wl_shm_interface, 1));
		}
	}
	static void
	global_remove(void *, struct wl_registry *, uint32_t)
	{}

	bool
	init(struct wl_display *d, uint32_t bw, uint32_t bh)
	{
		struct wl_registry *reg = wl_display_get_registry(d);
		static const struct wl_registry_listener kL = {global, global_remove};
		wl_registry_add_listener(reg, &kL, this);
		wl_display_roundtrip(d);
		wl_registry_destroy(reg);
		if (shm == nullptr) {
			return false;
		}
		w = bw;
		h = bh;
		const size_t size = (size_t)w * h * 4;
		const int fd = memfd_create("fake-wsi", MFD_CLOEXEC);
		if (fd < 0 || ftruncate(fd, (off_t)size) != 0) {
			return false;
		}
		struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
		buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t)w, (int32_t)h, (int32_t)w * 4,
		                                   WL_SHM_FORMAT_XRGB8888);
		wl_shm_pool_destroy(pool);
		close(fd);
		return buffer != nullptr;
	}
	void
	present(struct wl_surface *s)
	{
		wl_surface_attach(s, buffer, 0, 0);
		wl_surface_damage(s, 0, 0, (int32_t)w, (int32_t)h);
		wl_surface_commit(s);
	}
	void
	destroy()
	{
		if (buffer != nullptr) {
			wl_buffer_destroy(buffer);
		}
		if (shm != nullptr) {
			wl_shm_destroy(shm);
		}
	}
};

/*!
 * DXR_LW_TEST_WAYLAND_PANEL="x,y,w,h:name": with a multi-output compositor,
 * fullscreen onto the output at that device-pixel rect must be requested only
 * once the surface is mapped, and must land on the output named `name`.
 */
static void
test_wayland_panel_fullscreen(const char *spec)
{
	int px = 0, py = 0;
	unsigned pw = 0, ph = 0;
	char name[64] = {0};
	if (std::sscanf(spec, "%d,%d,%u,%u:%63s", &px, &py, &pw, &ph, name) != 5) {
		CHECK(false, "DXR_LW_TEST_WAYLAND_PANEL must be x,y,w,h:name");
		return;
	}
	DxrLinuxWindowDesc desc;
	desc.width = pw;
	desc.height = ph;
	desc.panel_left = px;
	desc.panel_top = py;
	desc.panel_width = pw;
	desc.panel_height = ph;
	desc.title = "linux_window_test (panel fullscreen)";
	desc.fullscreen_on_wayland = true;

	DxrLinuxWindow win;
	CHECK(win.create(DxrWindowBackend::Wayland, desc), "create Wayland window (panel fullscreen)");
	if (win.backend() != DxrWindowBackend::Wayland) {
		return;
	}
	CHECK(!win.is_fullscreen(), "fullscreen is deferred until the surface is mapped");
	uint32_t w = 0, h = 0;
	CHECK(win.current_size(&w, &h) && w == pw && h == ph, "the declared buffer is the panel mode from the start");

	const auto *bind =
	    static_cast<const XrWaylandSurfaceBindingCreateInfoDXR *>(win.session_binding_chain(nullptr));
	FakeWsi wsi;
	CHECK(bind != nullptr && wsi.init(bind->wlDisplay, w, h), "fake WSI");
	bool running = true;
	for (int i = 0; i < 80 && running; i++) {
		wsi.present(bind->wlSurface);
		win.pump_events({}, &running);
		usleep(10 * 1000);
	}
	CHECK(win.is_fullscreen(), "fullscreen once mapped");
	const std::string on = win.current_output_name();
	std::printf("panel fullscreen landed on '%s' (wanted '%s')\n", on.c_str(), name);
	CHECK(on == name, "the fullscreen surface is on the requested panel output");
	wsi.destroy(); // before the window: destroy() disconnects the display it lives on
	win.destroy();
}

static void
test_wayland_window()
{
	DxrLinuxWindowDesc desc;
	desc.width = 640;
	desc.height = 360;
	desc.title = "linux_window_test (wayland)";
	desc.fullscreen_on_wayland = false;
	desc.transparent = true;
	desc.wayland_drag_button = 3;

	DxrLinuxWindow win;
	CHECK(win.create(DxrWindowBackend::Wayland, desc), "create Wayland window");
	if (win.backend() != DxrWindowBackend::Wayland) {
		return;
	}
	CHECK(win.connection_description().rfind("Wayland", 0) == 0, "connection verified as Wayland");
	CHECK(win.is_transparent(), "transparent is native on Wayland");
	const auto *bind =
	    static_cast<const XrWaylandSurfaceBindingCreateInfoDXR *>(win.session_binding_chain(nullptr));
	CHECK(bind != nullptr && bind->type == XR_TYPE_WAYLAND_SURFACE_BINDING_CREATE_INFO_DXR, "wayland binding");
	if (bind != nullptr) {
		CHECK(bind->transparentBackgroundEnabled == XR_TRUE, "binding carries transparency");
		const auto *geo = static_cast<const XrWaylandSurfaceGeometryDXR *>(bind->next);
		CHECK(geo != nullptr && geo->type == XR_TYPE_WAYLAND_SURFACE_GEOMETRY_DXR && geo->width > 0,
		      "geometry struct chained after the binding");
	}
	uint32_t w = 0, h = 0;
	CHECK(win.current_size(&w, &h) && w > 0 && h > 0, "a declared size");
	bool running = true;
	for (int i = 0; i < 5; i++) {
		win.pump_events({}, &running);
	}
	const DxrWindowRect r = {10, 10, 100, 50};
	CHECK(win.set_input_region(&r, 1), "wayland input region");
	win.clear_input_region();
	win.set_title("retitled");
	win.set_keep_above(true); // logged no-op
	CHECK(win.toggle_fullscreen(), "F11 on Wayland");
	for (int i = 0; i < 5; i++) {
		win.pump_events({}, &running);
	}
	CHECK(running, "no close request");
	win.destroy();
}
#endif

int
main()
{
	test_parse();

	const DxrWindowProbe p = DxrLinuxWindow::probe(true);
	std::printf("probe: %s (Wayland-ready: %s)\n", p.describe().c_str(), p.wayland_ready() ? "yes" : "no");
	test_select(p);

	const char *x11 = std::getenv("DXR_LW_TEST_X11");
	if (x11 != nullptr && std::strcmp(x11, "1") == 0) {
		CHECK(p.x11_connects, "DXR_LW_TEST_X11=1 needs an X server (xvfb-run)");
		if (p.x11_connects) {
			test_x11_window(false);
			test_x11_window(true);
		}
	} else {
		std::printf("X11 window checks skipped (set DXR_LW_TEST_X11=1 under xvfb-run to run them)\n");
	}

#ifdef DXR_APP_HAVE_WAYLAND
	const char *wl = std::getenv("DXR_LW_TEST_WAYLAND");
	if (wl != nullptr && std::strcmp(wl, "1") == 0) {
		CHECK(p.wayland_connects, "DXR_LW_TEST_WAYLAND=1 needs a compositor (e.g. weston --backend=headless)");
		if (p.wayland_connects) {
			test_wayland_window();
			if (const char *panel = std::getenv("DXR_LW_TEST_WAYLAND_PANEL")) {
				test_wayland_panel_fullscreen(panel);
			}
		}
	}
#endif

	if (g_failures != 0) {
		std::fprintf(stderr, "linux_window_test: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("linux_window_test: OK\n");
	return 0;
}

// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  THE desktop-Linux app window for DisplayXR apps: X11 or native
 *         Wayland in one binary, chosen by capability at startup.
 *
 * One object that owns either an X11 toplevel (XR_DXR_xlib_window_binding) or
 * a native Wayland xdg-shell toplevel (XR_DXR_wayland_surface_binding), picked
 * at RUNTIME so a single binary works in either session. The window-binding
 * struct it hands back is chained onto XrSessionCreateInfo by the app.
 *
 * ONE IMPLEMENTATION. This is the official helper, consumed through the
 * `displayxr::linux_window` target by the runtime's Linux test apps and by
 * every demo. It was lifted verbatim out of the runtime's
 * test_apps/common/dxr_linux_window (where it was hardware-validated on a
 * 3840x2160 panel at 200 % scale); the demo-facing additions (input events,
 * the X11 header bar, transparency + click-through) are additive, so the
 * validated paths are unchanged. Never copy it into an app.
 *
 * BACKEND SELECTION (select(), and the one policy point in the .cpp):
 *   1. An explicit request (--platform=x11|wayland) always wins.
 *   2. Auto prefers NATIVE Wayland when the compositor is "Wayland-ready":
 *      it offers wp_fractional_scale_v1 + wp_viewporter AND the
 *      window-geometry extension owns its name on the session bus. (Measured:
 *      35% GPU native vs 60% through XWayland on an integrated GPU.)
 *   3. Otherwise X11 — connect to an X server, XWayland counts. This is what
 *      Ubuntu 22.04 (no fractional-scale protocol) and sessions without the
 *      extension get.
 *   4. Native Wayland is also the fallback when no X server answers. The
 *      verdict is LOGGED on every auto run, and the whole policy is one
 *      constant in the .cpp (kAutoPrefersReadyWayland).
 *   It NEVER reads WAYLAND_DISPLAY, XDG_SESSION_TYPE or any other environment
 *   variable to decide: the only questions asked are "does a connection
 *   succeed" and "what does the server advertise". (XOpenDisplay and
 *   wl_display_connect themselves read DISPLAY / WAYLAND_DISPLAY to find the
 *   socket — that is the library connecting, not this helper guessing.)
 *   After create(), the connection actually obtained is re-verified and
 *   logged (GLFW-style glfwGetPlatform), so a binary built against one
 *   library version and run against another reports what it really got.
 *
 * X11 PLACEMENT CONTRACT (INV-1.3, #729):
 *   A panel-sized window (desc.width/height == desc.panel_width/height) is made
 *   genuinely FULLSCREEN on the panel's monitor, because mutter discards
 *   client-requested geometry for such a window AND decorates it — the observed
 *   result of asking for 3840x2160+3456+0 was a 3840x2086 client at (3456, 74)
 *   under a mutter-x11-frames parent. Fullscreen is the one WM-cooperative
 *   placement primitive, and it is what makes window origin ≡ panel origin
 *   (which the weave phase depends on). The recipe, validated on GNOME 50 /
 *   XWayland: map -> XMoveWindow onto the target output -> pump events briefly
 *   -> EWMH _NET_WM_STATE_FULLSCREEN + _NET_WM_FULLSCREEN_MONITORS. Order
 *   matters: a position request before the window is mapped is discarded, and
 *   mutter fullscreens onto whichever output the window currently occupies.
 *   A windowed size keeps the old behaviour; DXR_X11_NO_FULLSCREEN=1 opts out.
 *
 * X11 CLIENT-OWNED DRAG (#1588):
 *   A WINDOWED X11 toplevel is undecorated too, and this helper — not the
 *   window manager — moves it. The reason is the weave: a WM-owned drag
 *   (mutter's _NET_WM_MOVERESIZE grab) cannot be intercepted by the client, so
 *   the window lands on an arbitrary pixel every frame and the lenticular
 *   interlace phase re-lands with it, which reads as a shimmer/stutter. Windows
 *   avoids this by snapping the window's position to the lens lattice DURING
 *   the drag (WM_WINDOWPOSCHANGING -> the DP's snap_window_rect); the only way
 *   to get the same hook under mutter is to own the drag. So: no decorations,
 *   a button-1 pointer grab anywhere in the window, and every move routed
 *   through the app-installed snap provider (see set_snap_provider(), which
 *   the cube apps back with xrWeaveSnapWindowRectDXR) before XMoveWindow.
 *   Post-map client moves ARE honoured by mutter (verified, #729).
 *   DXR_X11_WM_DECORATIONS=1 restores the decorated, WM-dragged window.
 *
 * ORDERING CONTRACT (both backends):
 *   create instance -> get system -> xrGetSystemProperties (panel rect, INV-1.3)
 *   -> DxrLinuxWindow::create() -> xrCreateSession with session_binding_chain()
 *   -> ... -> destroy() LAST, after the Vulkan instance is gone. The runtime's
 *   VkSurfaceKHR borrows the Xlib Display / wl_display connection for its
 *   lifetime, so the connection must outlive the session.
 *
 * WAYLAND WINDOW CHROME (#1654):
 *   GNOME's mutter gives Wayland clients no server-side decorations, so a
 *   WINDOWED native-Wayland toplevel gets a client-side title bar: the shared
 *   displayxr::csd painter (displayxr-common#52) in a wl_subsurface ABOVE the
 *   bound surface — see dxr_wl_chrome.h. The bound surface stays exactly the
 *   content rect (the bar never reaches the swapchain, the atlas or the
 *   weave); window geometry is bar + content, so xdg_toplevel.configure sizes
 *   are frame sizes and the helper subtracts the bar before declaring the
 *   content size. Fullscreen hides the bar. Server-side decorations are
 *   preferred where zxdg_decoration_manager_v1 offers them; DXR_WL_CSD=0
 *   turns the chrome off, DXR_WL_CSD=force draws it regardless.
 *
 * WAYLAND CONTRACT (see docs/specs/extensions/XR_DXR_wayland_surface_binding.md):
 *   - The surface must already have an xdg role and its first configure must be
 *     ACKED before xrCreateSession: the runtime calls vkCreateWaylandSurfaceKHR
 *     synchronously inside the session create
 *     (src/xrt/compositor/vk_native/comp_vk_native_target.cpp:1797) and Mesa's
 *     WSI attaches a buffer on the first present.
 *   - The app must NEVER attach a wl_buffer, commit, or install a
 *     wl_surface_frame callback once the session exists — the WSI owns those.
 *     This helper commits exactly once, during create(), before the session.
 *   - The runtime never pumps the Wayland queue (there is no wl_display_* call
 *     anywhere in src/), so pump() must be called every frame.
 *   - Fullscreen on the PANEL output is requested only once the surface is
 *     MAPPED (its first wl_surface.enter, i.e. after the WSI presented a
 *     frame): mutter discards the output argument of a set_fullscreen made
 *     before the first buffer. The one windowed frame before it is expected.
 *     Where the compositor actually put the fullscreen surface is logged
 *     against the requested output (MATCH / MISMATCH).
 *   - A wl_surface has NO intrinsic size: the WSI reports
 *     `currentExtent == UINT32_MAX` and the buffer the runtime attaches is what
 *     DEFINES the surface. So the app must DECLARE its size — this helper
 *     chains XrWaylandSurfaceGeometryDXR at session create and republishes
 *     through xrSetWaylandSurfaceGeometryDXR on every later configure that
 *     changes it (extension spec v2). Call attach_session() right after
 *     xrCreateSession to arm that; against an older runtime the function is
 *     simply absent and the helper logs once and stays quiet.
 */
#pragma once

#include "u_x11_scale.h" // placement-landing probe (shared with the runtime)
#include "csd_titlebar.h" // displayxr::csd — the X11 header bar's painter (and Wayland's, via dxr_wl_chrome)

// Xlib first: XR_DXR_xlib_window_binding.h wants the real Display / Window
// types, not its self-contained stand-ins.
#include <X11/Xlib.h>
#include <X11/Xutil.h> // XSizeHints for INV-1.3 window placement

#ifdef DXR_APP_HAVE_WAYLAND
// Likewise: the Wayland binding header forward-declares wl_display/wl_surface,
// so the real definitions have to come first.
#include <wayland-client.h>
#ifdef DXR_APP_HAVE_WL_CHROME
#include "dxr_wl_chrome.h"    // title bar for the native-Wayland leg (#1654)
#include "dxr_wl_placement.h" // drag lattice: a phase-snapped compositor drag (#1609)
#endif
#endif

#include <openxr/openxr.h>
#include <openxr/XR_DXR_xlib_window_binding.h>
#include <openxr/XR_DXR_wayland_surface_binding.h>

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

//! Which app-owned window backend (and therefore which binding extension).
enum class DxrWindowBackend
{
	Auto,   //!< pick from the session environment + what the runtime advertises
	X11,    //!< XR_DXR_xlib_window_binding
	Wayland //!< XR_DXR_wayland_surface_binding
};

/*!
 * Backend-neutral key identity. Only the handful of keys the Linux test apps
 * bind — the Wayland leg decodes raw evdev keycodes (no xkbcommon), so this is
 * deliberately not a general keysym.
 *
 * NOTE: the idle enumerator is `Unknown`, not `None`, because <X11/X.h> does
 * `#define None 0L` and would mangle the enumerator.
 */
enum class DxrKey
{
	Unknown,
	Escape,
	Q,
	M,
	O,
	V,
	Num1,
	Num2,
	Num3,
	F11 //!< handled INSIDE pump() (toggle_fullscreen); never reaches on_key
};

//! Modifier bits in DxrWindowEvent::mods — identical on both backends.
enum DxrKeyMod : uint32_t
{
	DxrModShift = 1u << 0,
	DxrModCtrl = 1u << 1,
	DxrModAlt = 1u << 2,
	DxrModSuper = 1u << 3,
};

/*!
 * One input or window event, backend-neutral, delivered by pump_events().
 *
 * COORDINATES are CONTENT-relative BUFFER pixels: the space of the swapchain
 * the runtime presents into and of current_size(). The header bar (either
 * backend) is never part of it, and on a scaled Wayland output the logical
 * surface coordinates are converted, so an app's picking maths is the same
 * on X11 and Wayland.
 *
 * KEYS carry the X11 keysym at shift level 0 — exactly what the X11 leg's
 * XLookupKeysym(ev, 0) returns — so an app written against <X11/keysym.h>
 * (XK_w, XK_F11, XK_bracketleft, …) works unchanged on Wayland. On Wayland it
 * comes from the compositor's keymap through libxkbcommon when the helper was
 * built with it, else from a built-in US-layout evdev table.
 */
struct DxrWindowEvent
{
	enum class Type
	{
		KeyDown,      //!< keysym/keycode/mods; `repeat` for auto-repeat presses
		KeyUp,        //!< never sent for auto-repeat (X11's fake releases are filtered)
		ButtonDown,   //!< button/x/y/mods
		ButtonUp,     //!< ...
		Motion,       //!< x/y; not sent while a window drag owns the pointer
		Scroll,       //!< scroll_steps (+1 per notch up) / scroll_steps_x; x/y
		PointerLeave, //!< the pointer left the content
		FocusGained,  //!< keyboard focus gained ("FocusIn" is an X11 macro)
		FocusLost,    //!< keyboard focus lost — release any held-key state
		Resize,       //!< width/height: the new CONTENT size in buffer px
	};
	Type type = Type::Motion;
	uint32_t keysym = 0;  //!< X11 keysym value (XK_*), shift level 0
	uint32_t keycode = 0; //!< X11/xkb keycode (evdev + 8), same numbering on both backends
	bool repeat = false;  //!< KeyDown generated by auto-repeat (key still held)
	uint32_t mods = 0;    //!< DxrKeyMod bits at the time of the event
	uint32_t button = 0;  //!< 1 left, 2 middle, 3 right, 8 back, 9 forward
	//! The press started a window drag (the configured drag button), or the
	//! release ended one. The app still sees it, so it can track button state,
	//! but must not treat it as a click into the scene.
	bool window_drag = false;
	int32_t x = 0, y = 0;         //!< pointer, CONTENT buffer px
	int32_t scroll_steps = 0;     //!< vertical notches, +1 = up / away from the user
	int32_t scroll_steps_x = 0;   //!< horizontal notches, +1 = right
	uint32_t width = 0, height = 0; //!< Resize only
	uint32_t time_ms = 0;         //!< server timestamp (for double-click detection)
};

//! A rectangle in CONTENT buffer pixels (see set_input_region()).
struct DxrWindowRect
{
	int32_t x = 0, y = 0;
	uint32_t width = 0, height = 0;
};

/*!
 * What the window systems on this box can actually do — the capability probe
 * behind select(). Every field is the result of a connection attempt or of a
 * server advertisement; none is read from the environment.
 */
struct DxrWindowProbe
{
	bool x11_connects = false;    //!< XOpenDisplay(NULL) succeeded
	bool x11_is_xwayland = false; //!< ...and the server exposes the XWAYLAND extension
	std::string x11_server;       //!< "vendor release" of that server

	bool wayland_compiled = false;   //!< the Wayland leg is compiled into this binary
	bool wayland_connects = false;   //!< wl_display_connect(NULL) succeeded
	bool wl_fractional_scale = false; //!< wp_fractional_scale_manager_v1 advertised
	bool wl_viewporter = false;       //!< wp_viewporter advertised

	bool dbus_available = false;   //!< libdbus-1 loaded and the session bus answered
	bool geometry_service = false; //!< org.displayxr.WindowGeometry is owned (the GNOME extension is live)

	//! The condition for preferring native Wayland once the drag fix ships:
	//! fractional scale + viewporter + our geometry extension on the bus.
	bool
	wayland_ready() const
	{
		return wayland_connects && wl_fractional_scale && wl_viewporter && geometry_service;
	}

	//! One line for the log.
	std::string
	describe() const;
};

//! What to create. Sizes are panel pixels; the panel rect comes from
//! XrDisplayDesktopPositionDXR + XrDisplayInfoDXR (INV-1.3).
struct DxrLinuxWindowDesc
{
	uint32_t width = 0;  //!< requested window size, pixels
	uint32_t height = 0; //!< requested window size, pixels

	int32_t panel_left = 0;    //!< 3D panel top-left in virtual-desktop pixels
	int32_t panel_top = 0;     //!< ...
	uint32_t panel_width = 0;  //!< 3D panel size in pixels (0 = unknown)
	uint32_t panel_height = 0; //!< ... a window of exactly this size goes
	                           //!< fullscreen on the panel's monitor (X11, #729)

	const char *title = "DisplayXR";               //!< toplevel title
	const char *app_id = "com.displayxr.test_app"; //!< Wayland xdg app-id

	//! Wayland only. Fullscreen-on-the-panel is the INV-1.3 substitute (a
	//! Wayland client cannot place itself), so it stays the default. Clearing
	//! it gives a windowed toplevel, which IS supported from extension spec
	//! v2: this helper declares `width`/`height` through
	//! XrWaylandSurfaceGeometryDXR so the runtime sizes its WSI swapchain to
	//! the surface instead of the panel. Note the weave PHASE still needs the
	//! compositor geometry service for a windowed surface — the size comes
	//! from here, the position does not.
	bool fullscreen_on_wayland = true;

	/*!
	 * Transparent background capability (both backends). X11: the window is
	 * created on a 32-bit ARGB visual (falls back to opaque when the screen
	 * has none — see is_transparent()); Wayland: nothing to do on the surface.
	 * Either way the binding's transparentBackgroundEnabled is set, which the
	 * runtime can only honour at xrCreateSession — so this is a create-time
	 * choice, and what the app DRAWS decides whether it looks transparent.
	 */
	bool transparent = false;

	/*!
	 * X11 only: a client-side header bar (displayxr::csd — the same painter
	 * as the Wayland chrome) on a windowed, client-dragged window. The
	 * top-level is bar + content, and the CONTENT is a child window: that
	 * child is what is bound to the runtime, so the bar never enters the
	 * swapchain, the atlas or the weave. LMB on the bar drives the same
	 * phase-snapped drag as the drag button. Off by default: the runtime's
	 * cube apps keep their undecorated, drag-anywhere window.
	 *
	 * (The native-Wayland leg always carries its title bar — mutter offers
	 * Wayland clients no server-side decorations — see dxr_wl_chrome.h.)
	 */
	bool x11_header_bar = false;

	//! X11: the button that drags a windowed, undecorated window from
	//! ANYWHERE in the content, through the phase snap. 1 = left (the cube
	//! apps' test affordance), 3 = right (the demos' convention, which keeps
	//! the left button for the scene), 0 = none (header bar only).
	uint32_t x11_drag_button = 1;

	//! Wayland: the button that starts a compositor move (xdg_toplevel.move)
	//! from anywhere in the content. 0 = none (the default: the title bar
	//! moves the window, as before); demos use 3 to match their X11 leg.
	uint32_t wayland_drag_button = 0;

	//! Keep above other windows from the start (X11 _NET_WM_STATE_ABOVE,
	//! set before the map). Wayland has no such protocol; ignored there.
	bool keep_above = false;

	//! X11 only, windowed only: where the CONTENT's top-left goes, in
	//! virtual-desktop px (a header bar sits above it). Default: the panel
	//! origin (panel_left, panel_top). A panel-sized window ignores it — it
	//! goes fullscreen on the panel. Wayland clients cannot place themselves.
	bool has_position = false;
	int32_t x = 0;
	int32_t y = 0;
};

/*!
 * App-owned toplevel window, X11 or Wayland.
 *
 * Not copyable; one instance per app. All state is owned inline (the binding
 * struct handed to xrCreateSession is member storage, so it stays alive for
 * the duration of the call).
 */
class DxrLinuxWindow
{
public:
	DxrLinuxWindow() = default;
	~DxrLinuxWindow();
	DxrLinuxWindow(const DxrLinuxWindow &) = delete;
	DxrLinuxWindow &operator=(const DxrLinuxWindow &) = delete;

	/*!
	 * Resolve `requested` against the session environment and what the runtime
	 * advertises. Never touches any connection — safe to call before create().
	 *
	 * @param runtime_has_xlib   XR_DXR_xlib_window_binding was enumerated
	 * @param runtime_has_wayland XR_DXR_wayland_surface_binding was enumerated
	 * @param reason             filled with a one-line human explanation (may be null)
	 * @return the chosen backend, or DxrWindowBackend::Auto when nothing is usable
	 *         (the caller should treat that as a hard error and print `reason`).
	 */
	static DxrWindowBackend
	select(DxrWindowBackend requested, bool runtime_has_xlib, bool runtime_has_wayland, std::string *reason);

	//! Parse "x11" / "wayland" / "auto"; returns false on anything else.
	static bool
	parse_backend(const char *text, DxrWindowBackend *out);

	/*!
	 * Scan argv for the platform request: `--platform=x11|wayland|auto`,
	 * `--platform x11|wayland|auto`, and the older spelling `--backend=…`.
	 * The LAST occurrence wins.
	 *
	 * @return false only for a malformed value (@p error says why); true
	 *         otherwise, with @p out left untouched when nothing was given.
	 */
	static bool
	parse_platform_args(int argc, char **argv, DxrWindowBackend *out, std::string *error);

	//! As parse_platform_args(), over an argument vector WITHOUT argv[0], and
	//! REMOVING what it consumed — so an app's own parser (e.g. dxr::
	//! ParseLaunchArgs) never sees `--platform x11` and takes "x11" for a
	//! positional path.
	static bool
	take_platform_args(std::vector<std::string> *args, DxrWindowBackend *out, std::string *error);

	/*!
	 * Run the capability probe: try both connections (and, when asked, the
	 * Wayland globals and the session bus), then disconnect. Never reads the
	 * environment to decide anything. Cheap enough to run once at startup;
	 * select() calls it.
	 */
	static DxrWindowProbe
	probe(bool wayland_details);

	static const char *
	backend_name(DxrWindowBackend b);

	//! Bring the window up. On Wayland this includes the full xdg-shell
	//! handshake: role, fullscreen request, commit, and the first
	//! xdg_surface.configure ACKED — the session may be created straight after.
	bool
	create(DxrWindowBackend backend, const DxrLinuxWindowDesc &desc);

	/*!
	 * Drain the window system's event queue. Call once per frame, never
	 * blocking.
	 *
	 * @param on_key  invoked for each key press (may be empty)
	 * @param running cleared when the user asks to close the window
	 */
	void
	pump(const std::function<void(DxrKey)> &on_key, bool *running);

	/*!
	 * The full event stream: keys (with keysyms + modifiers), buttons,
	 * motion, scroll, focus, pointer-leave and content resizes. Call once per
	 * frame instead of pump(on_key) — never both. F11 is still handled here
	 * (and still delivered, so an app can show state); window drags, the
	 * header bar and the close button are the helper's and are consumed.
	 *
	 * @param on_event invoked for each event (may be empty)
	 * @param running  cleared when the user asks to close the window
	 */
	void
	pump_events(const std::function<void(const DxrWindowEvent &)> &on_event, bool *running);

	//! Retitle the window (X11 WM_NAME + header bar; Wayland xdg title + bar).
	void
	set_title(const char *title);

	/*!
	 * Click-through: restrict where the window accepts pointer input to
	 * @p rects (CONTENT buffer px). Everywhere else clicks go to whatever is
	 * underneath. The header bar, when shown, is always added. X11 uses an
	 * XShape ShapeInput region on the top-level; Wayland sets the bound
	 * surface's input region (pending state, committed by the runtime's next
	 * present — the app never commits after xrCreateSession).
	 *
	 * @return false when the backend cannot express it (X11 server without
	 *         the SHAPE extension, or a build without libXext).
	 */
	bool
	set_input_region(const DxrWindowRect *rects, size_t count);

	//! Undo set_input_region(): the whole window accepts input again.
	void
	clear_input_region();

	//! The window really is transparent-capable (desc.transparent AND, on
	//! X11, an ARGB visual was available).
	bool
	is_transparent() const
	{
		return m_transparent;
	}

	//! Float above other windows (X11 _NET_WM_STATE_ABOVE); logged no-op on
	//! Wayland, which has no protocol for it.
	void
	set_keep_above(bool above);

	//! What the connection actually is, e.g. "X11 (XWayland, The X.Org
	//! Foundation 12401010)" or "Wayland (native)". Empty before create().
	const std::string &
	connection_description() const
	{
		return m_connection_desc;
	}

	//! Wayland only: the name of the output the surface is on, from
	//! wl_surface.enter + zxdg_output_v1.name ("" when unknown, on several
	//! outputs, or on X11). What the fullscreen placement log compares.
	std::string
	current_output_name() const;

	//! X11 only: the Display and the window BOUND to the runtime (the content
	//! child when the header bar is on). Null / 0 on Wayland.
	Display *
	x11_display() const
	{
		return m_x_display;
	}
	::Window
	x11_bound_window() const
	{
		return m_x_content != 0 ? m_x_content : m_x_window;
	}

	/*!
	 * F11: toggle fullscreen. Onto the 3D panel's output / RandR monitor when
	 * one was matched at create, else wherever the compositor puts it. pump()
	 * calls this itself on F11, on both backends, so apps need not.
	 *
	 * Wayland: xdg_toplevel.set_fullscreen(panel wl_output) / unset. The
	 * declared buffer follows (the panel's MODE while fullscreen on it, the
	 * windowed configure x scale after), and the title bar hides / returns.
	 * X11: _NET_WM_STATE_FULLSCREEN + _NET_WM_FULLSCREEN_MONITORS, and the
	 * client-owned drag is disabled while fullscreen.
	 *
	 * @return false when there is no window to toggle.
	 */
	bool
	toggle_fullscreen();

	bool
	is_fullscreen() const;

	//! A client-side header bar is currently shown (X11 x11_header_bar while
	//! windowed; the Wayland chrome while windowed and client-side). It is
	//! always kept clickable by set_input_region().
	bool
	header_bar_visible() const;

	//! Live window size in BUFFER pixels — the space the runtime's swapchain
	//! and every rect handed to it live in. X11 reads XGetWindowAttributes;
	//! Wayland returns the size declared through XrWaylandSurfaceGeometryDXR
	//! (the matched output's mode when fullscreen, else the last
	//! xdg_toplevel.configure), never the logical configure size of a
	//! fractionally-scaled fullscreen surface. False when no size is known
	//! yet — the caller should fall back to its own envelope.
	bool
	current_size(uint32_t *w, uint32_t *h) const;

	/*!
	 * Pointer to the filled window-binding struct, with `.next = next`, ready
	 * to hand to XrSessionCreateInfo::next. Member storage: valid until this
	 * object is destroyed. Returns `next` unchanged if no window exists.
	 *
	 * On Wayland the returned chain is TWO structs: the binding, followed by
	 * XrWaylandSurfaceGeometryDXR carrying the acked configure size and the
	 * matched output's refresh (spec v2). Without the second one the runtime
	 * would size its swapchain to the panel and thereby resize the surface.
	 */
	const void *
	session_binding_chain(const void *next);

	/*!
	 * Arm the mid-session geometry channel. Call once, right after
	 * xrCreateSession; no-op on X11.
	 *
	 * Resolves xrSetWaylandSurfaceGeometryDXR through xrGetInstanceProcAddr.
	 * An older runtime returns NULL for it — that is not an error, it just
	 * means the session is stuck with the size declared at create; the helper
	 * logs once and never asks again.
	 */
	void
	attach_session(XrInstance instance, XrSession session);

	/*!
	 * Republish the surface size to the runtime NOW, whatever the compositor
	 * last configured.
	 *
	 * pump() already does this for every real xdg_toplevel.configure, so apps
	 * do not need it. It exists for the test hook that exercises the runtime's
	 * resize-follow without a user dragging a window edge (see
	 * DXR_CUBE_TEST_RESIZE in the Linux cube apps).
	 *
	 * @return false when there is no Wayland session to publish to.
	 */
	bool
	force_declare_geometry(uint32_t width, uint32_t height);

	/*!
	 * Drag-time window-origin snap provider (#1588).
	 *
	 * A DELIBERATELY PLAIN function pointer: the snap math belongs to the
	 * vendor display processor and reaches this helper through the app's
	 * OpenXR session, but the helper itself must stay a window-system object
	 * — it does not know what a session is and never calls OpenXR for this.
	 *
	 * @param userdata    whatever was handed to set_snap_provider()
	 * @param origin_x/y  the window's root origin when the drag STARTED
	 *                    (the phase reference — the snap is absolute, and the
	 *                    DP wants to know where the travel began)
	 * @param target_x/y  the proposed new root origin, pointer-derived
	 * @param out_x/y     receives the lattice-snapped root origin
	 * @return false when nothing was snapped; the helper then moves to the
	 *         raw target and the out params are ignored.
	 */
	typedef bool (*SnapWindowOriginFn)(void *userdata,
	                                   int32_t origin_x,
	                                   int32_t origin_y,
	                                   int32_t target_x,
	                                   int32_t target_y,
	                                   int32_t *out_x,
	                                   int32_t *out_y);

	/*!
	 * Install (or clear, with @p fn nullptr) the snap provider. Call any time;
	 * with none installed every drag move is an identity snap, which is the
	 * correct behaviour against a runtime whose DP cannot snap — the drag
	 * mechanics are the same either way.
	 */
	void
	set_snap_provider(SnapWindowOriginFn fn, void *userdata);

	//! Name of the binding extension this backend needs enabled at
	//! xrCreateInstance, or nullptr when no window exists.
	const char *
	required_openxr_extension() const;

	DxrWindowBackend
	backend() const
	{
		return m_backend;
	}

	//! Human-readable identity of the bound window, for the session-create log.
	std::string
	describe() const;

	void
	destroy();

private:
	DxrWindowBackend m_backend = DxrWindowBackend::Auto;
	DxrLinuxWindowDesc m_desc = {};

	// --- Shared event plumbing ---------------------------------------------
	//! Transparent-capable for real (see is_transparent()).
	bool m_transparent = false;
	//! What create() actually connected to (verify_connection()).
	std::string m_connection_desc;
	//! Events queued by the backend listeners, drained by pump_events().
	std::vector<DxrWindowEvent> m_events;
	//! Current DxrKeyMod state (Wayland: from wl_keyboard.modifiers).
	uint32_t m_mods = 0;
	//! Last CONTENT size reported through a Resize event.
	uint32_t m_reported_w = 0, m_reported_h = 0;
	//! One-shot guard for the set_keep_above() Wayland no-op log.
	bool m_warned_keep_above = false;

	//! Log (and remember) what the live connection turned out to be.
	void
	verify_connection(DxrWindowBackend requested);
	//! Queue a Resize event when the content size changed since the last one.
	void
	note_content_size();
	//! Drain the backend and forward everything to @p on_event.
	void
	pump_impl(const std::function<void(const DxrWindowEvent &)> &on_event, bool *running);

	// --- X11 leg -----------------------------------------------------------
	Display *m_x_display = nullptr;
	//! The TOP-LEVEL window (the one the WM manages).
	::Window m_x_window = 0;
	Atom m_x_wm_delete = 0;
	//! The CONTENT child when the header bar is on (0 otherwise). It is the
	//! window bound to the runtime; see x11_bound_window().
	::Window m_x_content = 0;
	Colormap m_x_colormap = 0;          //!< ARGB colormap, freed with the window
	Visual *m_x_visual = nullptr;       //!< the top-level's visual (the bar packs pixels for it)
	bool m_x_bar_enabled = false;       //!< header bar built (windowed, client-dragged at some point)
	dxr_csd::TitleBar m_x_bar;          //!< the bar's painter + hit test
	int m_x_content_off_applied = -1;   //!< the child's current y inside the top-level
	uint32_t m_x_top_w = 0, m_x_top_h = 0;         //!< top-level size (last ConfigureNotify)
	uint32_t m_x_content_w = 0, m_x_content_h = 0; //!< content size (bar excluded)
	unsigned int m_x_drag_btn = 0;      //!< button that started the current drag
	bool m_x_drag_from_bar = false;     //!< ...and it was a press on the header bar
	int m_x_drag_off_x = 0;             //!< bound-window origin minus top-level origin, at the grab
	int m_x_drag_off_y = 0;             //!< ...
	int m_x_shape_state = 0;            //!< 0 unknown, 1 available, -1 unavailable
	std::bitset<256> m_x_keys_down;     //!< held keycodes (auto-repeat detection)
	bool m_x_detectable_repeat = false; //!< XkbSetDetectableAutoRepeat took
	bool m_x_keep_above = false;

	bool
	x11_bar_visible() const;
	int
	x11_content_offset_y() const;
	//! Fit the content child to (0, bar) .. (W, H) of the top-level.
	void
	x11_layout_content();
	void
	x11_begin_drag(int root_x, int root_y, unsigned int button);
	void
	x11_end_drag();
	void
	x11_paint_bar();
	bool
	x11_shape_available();

	// --- Snap provider (#1588) ---------------------------------------------
	SnapWindowOriginFn m_snap_fn = nullptr;
	void *m_snap_userdata = nullptr;
	//! One-shot: the first move of the first drag says whether anything snaps.
	bool m_snap_reported = false;

	// --- X11 client-owned drag (#1588) --------------------------------------
	//! Windowed run with the WM's decorations + WM drag left in place
	//! (DXR_X11_WM_DECORATIONS=1). No client drag then.
	bool m_x_wm_drag = false;
	/*!
	 * This window owns its drag: windowed AND undecorated. FALSE for a
	 * fullscreen window as well as for the DXR_X11_WM_DECORATIONS opt-out —
	 * a fullscreen window must not be draggable at all, or a stray click
	 * would slide the panel-sized weave off the panel.
	 */
	bool m_x_client_drag = false;
	//! Fullscreen now (created panel-sized, or toggled by F11).
	bool m_x_fullscreen = false;
	bool m_x_dragging = false;
	int m_x_drag_ptr_x = 0;    //!< pointer root position at the grab
	int m_x_drag_ptr_y = 0;    //!< ...
	int m_x_drag_origin_x = 0; //!< window root origin at the grab (snap origin)
	int m_x_drag_origin_y = 0; //!< ...
	int m_x_drag_at_x = 0;     //!< where the window was last moved to
	int m_x_drag_at_y = 0;     //!< ...
	uint64_t m_x_drag_moves = 0;    //!< XMoveWindow calls this drag
	uint64_t m_x_drag_snapped = 0;  //!< ...of which the snap changed the point

        /*!
         * Landing check: did the window go where the snap asked? A move is
         * asynchronous, so the previous request is compared against the
         * window's real origin just before the next move is issued (a pump
         * later, by which time the server has placed it). Accumulates across
         * drags; reports once. This is what would have caught the XWayland 2 px
         * quantum in minutes: a snap the environment silently rounds away is
         * indistinguishable from a working one unless someone reads the window
         * back.
         */
        u_x11_placement_probe m_x_probe = {};
        bool m_x_probe_pending = false;
        int m_x_probe_want_x = 0;
        int m_x_probe_want_y = 0;
        void x11_check_landing();

        // --- X11 programmatic drag test hook (DXR_X11_TEST_DRAG, #1588) ---------
	// Off by default. Walks the window along a straight path through the very
	// same snap -> XMoveWindow code the pointer drag uses, so the mechanics
	// are verifiable with nobody at the mouse. Not a fake X event: it drives
	// the same code path one step per pump().
	bool m_x_test_drag_armed = false;
	bool m_x_test_drag_done = false;
	int m_x_test_drag_dx = 0;
	int m_x_test_drag_dy = 0;
	int m_x_test_drag_steps = 0;
	int m_x_test_drag_step = 0;
	uint64_t m_x_pump_count = 0;
	uint64_t m_test_fs_pumps = 0; //!< DXR_TEST_FULLSCREEN_TOGGLE counter

	//! Run the snap provider, or identity when there is none / it declines.
	//! Reports once, the first time it is asked, what it resolved to.
	void
	snap_origin(int origin_x, int origin_y, int target_x, int target_y, int *out_x, int *out_y);

	//! snap_origin() + XMoveWindow, skipping a move that would not change the
	//! window's position. Logs only when the snap actually moved the point.
	void
	x11_move_snapped(int target_x, int target_y);

	//! One step of DXR_X11_TEST_DRAG, called from pump().
	void
	x11_drive_test_drag();

	// --- Binding storage (handed to xrCreateSession) ------------------------
	XrXlibWindowBindingCreateInfoDXR m_xlib_binding = {};
	XrWaylandSurfaceBindingCreateInfoDXR m_wl_binding = {};
	XrWaylandSurfaceGeometryDXR m_wl_geometry = {};

	// --- Mid-session geometry channel (spec v2) -----------------------------
	XrSession m_session = XR_NULL_HANDLE;
	PFN_xrSetWaylandSurfaceGeometryDXR m_pfn_set_wl_geometry = nullptr;
	//! Last size actually published, so pump() only calls on a real change.
	uint32_t m_wl_published_w = 0;
	uint32_t m_wl_published_h = 0;

	//! Push the current configure size to the runtime when it differs from
	//! what was last published. Cheap; safe to call every frame.
	void
	publish_wayland_geometry_if_changed();

#ifdef DXR_APP_HAVE_WAYLAND
	// --- Wayland leg -------------------------------------------------------
	/*!
	 * One output, with EVERY field's coordinate space in its name (#1596).
	 *
	 * The bug this naming exists to prevent: the panel match used to compare
	 * `wl_output.geometry`'s LOGICAL origin against the runtime's DEVICE-pixel
	 * panel rect, so on the measured box it compared 1728 against 3456 and
	 * could never succeed — the app then fullscreened on whatever output the
	 * compositor picked (the laptop) and wove into a resample.
	 */
	struct WlOutput
	{
		struct wl_output *output = nullptr;
		//! zxdg_output_v1, when the compositor advertises the manager. It is
		//! the ONLY source of an output's logical SIZE, and therefore the only
		//! way to derive a fractional scale (mode / logical_size).
		struct zxdg_output_v1 *xdg_output = nullptr;
		uint32_t name = 0;

		//! Position in the global LOGICAL layout. Seeded from
		//! wl_output.geometry, superseded by xdg_output.logical_position.
		int32_t logical_x = 0, logical_y = 0;
		//! Size in LOGICAL px, from xdg_output.logical_size only.
		int32_t logical_w = 0, logical_h = 0;
		bool have_logical_size = false;

		//! Current mode, DEVICE px (wl_output.mode). Directly comparable with
		//! the runtime's panel size, which is device px by contract.
		int32_t mode_w = 0, mode_h = 0;

		/*!
		 * wl_output.scale. An INTEGER by protocol, and therefore NOT the
		 * conversion factor: this box's 1.6667 laptop advertises 2. Kept only
		 * so a diagnostic can say what the compositor claimed; every
		 * conversion goes through u_wl_monitor_scale().
		 */
		int32_t int_scale = 1;

		int32_t refresh_mhz = 0; //!< wl_output.mode refresh, milli-hertz

		//! zxdg_output_v1.name (e.g. "HDMI-1"), for the placement log.
		std::string name_str;
	};

#ifdef DXR_APP_HAVE_WL_CHROME
	//! Title bar (client-side decorations) — see dxr_wl_chrome.h, #1654.
	DxrWlChrome m_wl_chrome;

	/*!
	 * @name Drag lattice (#1609)
	 *
	 * The compositor runs the title-bar drag (xdg_toplevel.move), and the
	 * window first hands it a table of phase-correct displacements to snap
	 * every proposed position to — see dxr_wl_placement.h.
	 * @{
	 */
	DxrWlPlacement m_wl_placement;
	//! Output scale of the drag in progress (the reachable step, device px).
	uint32_t m_wl_lattice_q = 0;
	//! Built for the drag in progress; answers DragLatticeNeeded while it runs.
	bool m_wl_lattice_active = false;
	//! The drag origin the compositor recorded (frame top-left, logical).
	int32_t m_wl_lattice_start_x = 0, m_wl_lattice_start_y = 0;
	//! DXR_WL_TEST_LATTICE state.
	uint64_t m_wl_test_lattice_pumps = 0;
	bool m_wl_test_lattice_done = false;

	//! Chrome hook: derive + send the table, just before the grab starts.
	void
	wl_drag_prepare();
	/*!
	 * Probe the display processor over a grid of displacements centred on
	 * (@p cx, @p cy) LOGICAL px and send the phase-correct, reachable ones.
	 * @return false when there is nothing to constrain (the DP declined, or
	 *         accepts every position) or the compositor refused the table.
	 */
	bool
	wl_send_lattice(bool extend, int32_t cx, int32_t cy);
	/*! @} */
#endif

	//! Logical -> device scale of the surface, for the chrome's raster.
	double
	wl_surface_scale() const
	{
		return m_wl_pref_scale_120 > 0 ? (double)m_wl_pref_scale_120 / 120.0 : 1.0;
	}

	struct wl_display *m_wl_display = nullptr;
	struct wl_registry *m_wl_registry = nullptr;
	struct wl_compositor *m_wl_compositor = nullptr;
	struct xdg_wm_base *m_wl_wm_base = nullptr;
	struct wl_seat *m_wl_seat = nullptr;
	//! zxdg_output_manager_v1 (#1596). Absent on a compositor that does not
	//! advertise it — the match then falls back to mode size alone, which is
	//! still device-vs-device and still correct, just without the origin as a
	//! tie-break.
	struct zxdg_output_manager_v1 *m_wl_xdg_output_manager = nullptr;
	struct wl_keyboard *m_wl_keyboard = nullptr;
	/*!
	 * Buffer -> surface mapping. The buffer the runtime attaches is in DEVICE
	 * pixels (logical size x output scale); without a mapping the compositor
	 * treats buffer pixels as LOGICAL pixels, so at 200 % a panel-mode buffer
	 * becomes a surface twice the output and spills onto the next monitor.
	 * wp_viewport's destination (= the configure size) is the mapping that
	 * works at any scale; set_buffer_scale is the integer-only fallback.
	 * wp_fractional_scale_v1 supplies a windowed surface's preferred scale.
	 * All optional; NULL when the compositor does not advertise them.
	 */
	struct wp_viewporter *m_wl_viewporter = nullptr;
	struct wp_viewport *m_wl_viewport = nullptr;
	struct wp_fractional_scale_manager_v1 *m_wl_frac_manager = nullptr;
	struct wp_fractional_scale_v1 *m_wl_frac = nullptr;
	//! wp_fractional_scale_v1.preferred_scale, in 120ths; 0 = not received.
	uint32_t m_wl_pref_scale_120 = 0;
	//! Mapping last applied to the surface (so it is re-sent only on change).
	int32_t m_wl_map_dst_w = 0, m_wl_map_dst_h = 0, m_wl_map_buffer_scale = 1;
	struct wl_surface *m_wl_surface = nullptr;
	struct xdg_surface *m_wl_xdg_surface = nullptr;
	struct xdg_toplevel *m_wl_toplevel = nullptr;
	std::vector<WlOutput> m_wl_outputs;

	bool m_wl_configured = false;
	//! CONTENT size, LOGICAL px: the last xdg_toplevel.configure minus the
	//! title bar (#1654) — the size of the bound surface, never the frame.
	int32_t m_wl_config_w = 0;
	int32_t m_wl_config_h = 0;
	//! Refresh of the output the surface went fullscreen on (0 = unknown).
	uint32_t m_wl_refresh_mhz = 0;
	/*!
	 * Mode size, in DEVICE pixels, of the output this surface went fullscreen
	 * on; 0 when windowed or when no output matched.
	 *
	 * This is what a fullscreen surface must declare, and it is NOT the
	 * configure size. xdg_toplevel.configure is in LOGICAL units, so on a
	 * fractionally-scaled desktop (this box runs 166.67%) a fullscreen
	 * toplevel is configured at 1728x1080 while the panel is 2880x1800. What
	 * reaches the panel 1:1 is a buffer of the output's MODE size: the
	 * compositor maps that whole buffer onto the output, so buffer pixels and
	 * panel pixels line up exactly. Declaring the logical size instead would
	 * hand the weaver a 1728x1080 image for Mutter to upscale — a resample,
	 * which destroys the interlace.
	 */
	int32_t m_wl_fullscreen_mode_w = 0;
	int32_t m_wl_fullscreen_mode_h = 0;

	//! The wl_output matched to the 3D panel at create (NULL when none), and
	//! its mode — the F11 target and its 1:1 buffer size.
	struct wl_output *m_wl_panel_output = nullptr;
	int32_t m_wl_panel_mode_w = 0, m_wl_panel_mode_h = 0;
	//! Fullscreen now, per the last sized configure (or the create request).
	bool m_wl_fullscreen = false;
	//! We sent unset_fullscreen and have not seen its configure yet.
	bool m_wl_unfullscreen_pending = false;
	//! The current / last fullscreen request targeted the panel output.
	bool m_wl_fs_on_panel = true;
	//! Last windowed CONTENT size, logical — where leaving fullscreen returns.
	int32_t m_wl_windowed_w = 0, m_wl_windowed_h = 0;

	//! Apply a fullscreen state change from a configure. @p cfg_w/h is the
	//! configure size (may be 0x0 = "you choose").
	void
	wl_set_fullscreen_state(bool fs, int32_t *cfg_w, int32_t *cfg_h);

	//! Size to declare to the runtime: the fullscreen output mode when there
	//! is one, else the acked configure size.
	void
	wl_declared_size(uint32_t *w, uint32_t *h) const;

	//! Map the declared (device-pixel) buffer onto the configure (logical)
	//! size: wp_viewport destination, else an integer set_buffer_scale.
	//! Pending surface state only — the WSI's next present commits it.
	void
	wl_apply_buffer_mapping();

	// Per-frame scratch, set by the listeners and consumed by pump().
	bool m_wl_close_requested = false;

	/*
	 * Fullscreen-on-the-panel ORDERING. mutter DISCARDS the output argument
	 * of xdg_toplevel.set_fullscreen when the surface has not committed a
	 * buffer yet: it keeps the fullscreen state and puts the window on
	 * whichever monitor it considers current when the first buffer arrives
	 * (pointer / focus — so it looks random). Measured under
	 * `gnome-shell --headless` with two virtual monitors: requesting before
	 * the first buffer landed on the wrong output 3/3, requesting after one
	 * presented frame on the right one 3/3. So the request is DEFERRED until
	 * the surface is mapped, which the first wl_surface.enter proves (the
	 * runtime's WSI presented a buffer). The swapchain is still declared at
	 * the panel's mode from the start, so the fullscreen transition changes
	 * nothing the runtime sized.
	 */
	bool m_wl_fs_deferred = false;   //!< set_fullscreen(panel) still owed
	uint64_t m_wl_pumps = 0;         //!< pump count (the deferral's timeout clock)
	//! Outputs the surface is on now (wl_surface.enter / leave).
	std::vector<struct wl_output *> m_wl_entered;
	//! Pumps left before the fullscreen-output verdict is logged; -1 = none due.
	int m_wl_output_report_in = -1;
	//! The last verdict logged, so an unchanged placement is not re-logged.
	std::string m_wl_output_reported;

	//! Request fullscreen on the panel output now (and log it).
	void
	wl_request_panel_fullscreen(const char *why);
	//! "HDMI-1 3840x2160" style label of an output, for the log.
	std::string
	wl_output_label(const struct wl_output *o) const;
	//! Log where the compositor actually put the fullscreen surface.
	void
	wl_report_fullscreen_output();
	//! The name of the output the surface is on ("" when unknown / several).
	std::string
	wl_current_output_name() const;
	static void
	s_surface_enter(void *data, struct wl_surface *s, struct wl_output *o);
	static void
	s_surface_leave(void *data, struct wl_surface *s, struct wl_output *o);

	// Input (the pointer itself is DxrWlChrome's; it forwards content events).
	void *m_wl_xkb_ctx = nullptr;    //!< struct xkb_context * (libxkbcommon builds)
	void *m_wl_xkb_keymap = nullptr; //!< struct xkb_keymap *
	void *m_wl_xkb_state = nullptr;  //!< struct xkb_state *
	double m_wl_ptr_x = 0.0, m_wl_ptr_y = 0.0; //!< last content pointer position, LOGICAL
	int32_t m_wl_discrete_x = 0, m_wl_discrete_y = 0; //!< this frame's wheel clicks
	double m_wl_axis_x = 0.0, m_wl_axis_y = 0.0;      //!< continuous scroll accumulators
	bool m_wl_frame_discrete = false;
	uint32_t m_wl_axis_time = 0;

	//! Content pointer events forwarded by the chrome (dxr_wl_chrome.h).
	static void
	s_content_pointer(void *userdata, const struct DxrWlPointerEvent &ev);
	void
	wl_content_pointer(const struct DxrWlPointerEvent &ev);
	//! Level-0 keysym for an evdev keycode (xkb keymap, else the US table).
	uint32_t
	wl_keysym(uint32_t evdev_key) const;
	//! Surface-local LOGICAL -> content BUFFER px.
	void
	wl_to_buffer(double lx, double ly, int32_t *bx, int32_t *by) const;

	bool
	create_wayland(const DxrLinuxWindowDesc &desc);
	void
	destroy_wayland();

	//! Give an output its zxdg_output_v1 once the manager exists. Idempotent,
	//! and called from both directions (output-first and manager-first) so
	//! registry ordering cannot leave an output without a logical size.
	void
	wl_attach_xdg_output(WlOutput &out);

	// Static trampolines (wayland-client listeners are C function pointers).
	static void
	s_registry_global(void *data, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version);
	static void
	s_registry_global_remove(void *data, struct wl_registry *r, uint32_t name);
	static void
	s_wm_base_ping(void *data, struct xdg_wm_base *b, uint32_t serial);
	static void
	s_xdg_surface_configure(void *data, struct xdg_surface *s, uint32_t serial);
	static void
	s_frac_preferred_scale(void *data, struct wp_fractional_scale_v1 *f, uint32_t scale_120);
	static void
	s_toplevel_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *states);
	static void
	s_toplevel_close(void *data, struct xdg_toplevel *t);
	static void
	s_toplevel_configure_bounds(void *data, struct xdg_toplevel *t, int32_t w, int32_t h);
	static void
	s_toplevel_wm_capabilities(void *data, struct xdg_toplevel *t, struct wl_array *caps);
	static void
	s_output_geometry(void *data,
	                  struct wl_output *o,
	                  int32_t x,
	                  int32_t y,
	                  int32_t pw,
	                  int32_t ph,
	                  int32_t subpixel,
	                  const char *make,
	                  const char *model,
	                  int32_t transform);
	static void
	s_output_mode(void *data, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t refresh);
	static void
	s_output_done(void *data, struct wl_output *o);
	static void
	s_output_scale(void *data, struct wl_output *o, int32_t factor);
	static void
	s_output_name(void *data, struct wl_output *o, const char *name);
	static void
	s_output_description(void *data, struct wl_output *o, const char *desc);
	static void
	s_xdg_output_logical_position(void *data, struct zxdg_output_v1 *o, int32_t x, int32_t y);
	static void
	s_xdg_output_logical_size(void *data, struct zxdg_output_v1 *o, int32_t w, int32_t h);
	static void
	s_xdg_output_done(void *data, struct zxdg_output_v1 *o);
	static void
	s_xdg_output_name(void *data, struct zxdg_output_v1 *o, const char *name);
	static void
	s_xdg_output_description(void *data, struct zxdg_output_v1 *o, const char *desc);
	static void
	s_seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps);
	static void
	s_seat_name(void *data, struct wl_seat *seat, const char *name);
	static void
	s_kb_keymap(void *data, struct wl_keyboard *kb, uint32_t format, int32_t fd, uint32_t size);
	static void
	s_kb_enter(void *data, struct wl_keyboard *kb, uint32_t serial, struct wl_surface *s, struct wl_array *keys);
	static void
	s_kb_leave(void *data, struct wl_keyboard *kb, uint32_t serial, struct wl_surface *s);
	static void
	s_kb_key(void *data, struct wl_keyboard *kb, uint32_t serial, uint32_t time, uint32_t key, uint32_t state);
	static void
	s_kb_modifiers(void *data,
	               struct wl_keyboard *kb,
	               uint32_t serial,
	               uint32_t depressed,
	               uint32_t latched,
	               uint32_t locked,
	               uint32_t group);
	static void
	s_kb_repeat_info(void *data, struct wl_keyboard *kb, int32_t rate, int32_t delay);
#endif // DXR_APP_HAVE_WAYLAND

	bool
	create_x11(const DxrLinuxWindowDesc &desc);
	void
	destroy_x11();
};

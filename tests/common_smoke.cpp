// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Smoke test for displayxr::common (epic #396 W4).
 *
 * Runs on the bare CI runners (no GPU, no OpenXR runtime): exercises the pure
 * helpers (capture filename numbering, mip-chain generation), instantiates the
 * platform structs so the full header chain (incl. the DisplayXR extension
 * headers) compiles, and — by linking at all — proves the target's link
 * closure (d3d11/d2d1/dwrite on Windows, the AppKit/CoreText frameworks on
 * macOS) and the STB single-implementation-per-platform invariant.
 */

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>
#include <vector>

#include "atlas_capture.h"
#include "auto_fit.h"
#include "auto_fit_canvas.h"
#include "clip_policy.h"
#include "content_bounds.h"
#include "dxr_view_math.h"
#include "mip_chain.h"
#include "mode_switch.h"
#include "view_params.h"
#include "xr_window_space_hud.h"

#ifdef _WIN32
#include "d3d11_renderer.h"
#include "hud_renderer.h"
#include "input_handler.h"
#include "text_overlay.h"
#include "window_manager.h"
#include "xr_session_common.h"
#endif

#ifdef __APPLE__
#include "hud_renderer_macos.h"
#include "stb_image.h"
#include "stb_image_write.h"
#endif

static int g_failures = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static void test_capture_numbering()
{
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "displayxr_common_smoke";
    fs::remove_all(dir);
    fs::create_directories(dir);

    // Empty dir → first capture is 1.
    CHECK(dxr_capture::NextCaptureNum(dir.string(), "smoke", 2, 1) == 1,
          "NextCaptureNum on empty dir should be 1");

    // Plant legacy-readback-style names; numbering is per (cols×rows).
    std::ofstream(dir / "smoke-3_2x1.png") << "x";
    std::ofstream(dir / "smoke-7_4x2.png") << "x";
    CHECK(dxr_capture::NextCaptureNum(dir.string(), "smoke", 2, 1) == 4,
          "NextCaptureNum should be max(N)+1 for matching cols x rows");
    CHECK(dxr_capture::NextCaptureNum(dir.string(), "smoke", 4, 2) == 8,
          "NextCaptureNum should track the 4x2 namespace separately");

    // The runtime-owned path (#425 naming): prefix numbers against
    // "<stem>-<N>_atlas_<viewCount>_<cols>x<rows>.png" and returns
    // "<dir>/<stem>-<N>" — no "_atlas", no extension.
    std::string prefix = dxr_capture::MakeCaptureAtlasPrefix("smoke", 2, 1);
    CHECK(!prefix.empty(), "MakeCaptureAtlasPrefix should not be empty");
    CHECK(prefix.find("_atlas") == std::string::npos,
          "MakeCaptureAtlasPrefix must not contain the runtime-owned _atlas token");
    CHECK(prefix.find("smoke-") != std::string::npos,
          "MakeCaptureAtlasPrefix should contain '<stem>-<N>'");

    fs::remove_all(dir);
}

static void test_mip_chain()
{
    // 4x4 solid mid-gray → 3 levels (4x4, 2x2, 1x1), box filter preserves value.
    std::vector<unsigned char> src(4 * 4 * 4, 128);
    auto mips = dxr_mip::GenerateMipChainRGBA8(src.data(), 4, 4);
    CHECK(mips.size() == 3, "4x4 source should yield 3 mip levels");
    if (mips.size() == 3) {
        CHECK(mips[2].width == 1 && mips[2].height == 1, "last mip should be 1x1");
        CHECK(mips[2].pixels[0] == 128, "box filter of a solid image preserves the value");
    }
}

// Load-time auto-fit (auto_fit.h). Pure, deterministic.
static void test_auto_fit()
{
	const float EPS = 1e-5f;
	auto near_eq = [&](float a, float b) { return a > b - EPS && a < b + EPS; };

	// Tall asset in a landscape viewport: height binds. vh = H / fill.
	CHECK(near_eq(dxr::AutoFitVHeight(0.5f, 1.8f, 1920.0f, 1080.0f), 1.8f / 0.8f),
	      "tall asset in landscape viewport must be height-bound");

	// Wide asset in a portrait viewport: width binds. vh = W / (fill * aspect).
	// aspect = 600/800 = 0.75 -> vh = 1.2 / (0.8 * 0.75) = 2.0.
	CHECK(near_eq(dxr::AutoFitVHeight(1.2f, 0.6f, 600.0f, 800.0f), 2.0f),
	      "wide asset in portrait viewport must be width-bound");

	// Exactly viewport-shaped asset: both axes land on fill together.
	CHECK(near_eq(dxr::AutoFitVHeight(1.6f, 0.9f, 1600.0f, 900.0f), 0.9f / 0.8f),
	      "viewport-shaped asset: width and height constraints coincide");

	// The result must always satisfy BOTH caps: rendered fractions <= fill.
	{
		const float vh = dxr::AutoFitVHeight(0.944f, 1.7f, 600.0f, 789.0f);
		const float aspect = 600.0f / 789.0f;
		CHECK(1.7f / vh <= 0.8f + EPS, "height fraction must not exceed fill");
		CHECK(0.944f / (vh * aspect) <= 0.8f + EPS, "width fraction must not exceed fill");
	}

	// Unknown viewport (0 x 0) degrades to the height-only fit.
	CHECK(near_eq(dxr::AutoFitVHeight(9.0f, 1.0f, 0.0f, 0.0f), 1.0f / 0.8f),
	      "unknown viewport must fall back to height-only fit");

	// Non-positive height or fill -> 0 (caller's fallback guard takes over).
	CHECK(dxr::AutoFitVHeight(1.0f, 0.0f, 100.0f, 100.0f) == 0.0f,
	      "zero extent height must return 0");
	CHECK(dxr::AutoFitVHeight(1.0f, 1.0f, 100.0f, 100.0f, 0.0f) == 0.0f,
	      "zero fill must return 0");

	// Custom fill fraction respected.
	CHECK(near_eq(dxr::AutoFitVHeight(0.5f, 1.0f, 1000.0f, 1000.0f, 0.5f), 2.0f),
	      "custom fill fraction must scale the fit");
}

// PanelPixelsFromView: panel = view_px / view_scale, pinned to numbers
// measured on real 3D-display hardware (2026-08-24). That panel is 2560x1600
// and its 3D mode is 2x1 tiles at scale 0.750x0.750, giving a 1920x1200
// per-view recommended rect -- the configuration that motivated this helper,
// because 0.750 != 1/2 so the tile grid does not cancel the view scale.
static void test_panel_px_from_view()
{
	// NOTE the fabs form. The `a > b - EPS && a < b + EPS` idiom used by
	// test_auto_fit above works only for small magnitudes: these values are
	// pixel counts, and in float32 the spacing at 2560 is ~2.4e-4, so
	// `2560.0f - 1e-4f` rounds straight back to 2560.0f and the strict `>`
	// then compares 2560 > 2560 and fails on an EXACT match.
	constexpr float EPS = 1e-3f;
	auto near_eq = [&](float a, float b) { return std::fabs(a - b) <= EPS; };
	float w = 0.0f, h = 0.0f;

	// The case that motivated this helper.
	CHECK(dxr::PanelPixelsFromView(1920u, 1200u, 0.75f, 0.75f, w, h),
	      "PanelPixelsFromView accepts the 2x1 @ 0.75 mode");
	CHECK(near_eq(w, 2560.0f) && near_eq(h, 1600.0f),
	      "1920x1200 @ 0.75 -> the real 2560x1600 panel");
	// ... and the aspect the fit rule actually consumes.
	CHECK(near_eq(w / h, 1.6f), "panel aspect is 1.600, not the atlas' 3.200");

	// The tile reconstruction this replaces would have produced 3840x1200.
	// Prove the two really do disagree, so a regression cannot pass silently.
	CHECK(!near_eq(w, 1920.0f * 2.0f),
	      "panel width is NOT per-view width x tile columns");

	// An isotropically-scaled 2D mode (1x1, scale 1.0) is the identity case.
	CHECK(dxr::PanelPixelsFromView(2560u, 1600u, 1.0f, 1.0f, w, h) &&
	          near_eq(w, 2560.0f) && near_eq(h, 1600.0f),
	      "scale 1.0 returns the view rect unchanged");

	// Anamorphic tiling (the shape the old reconstruction assumed) also works.
	CHECK(dxr::PanelPixelsFromView(1920u, 2160u, 0.5f, 1.0f, w, h) &&
	          near_eq(w, 3840.0f) && near_eq(h, 2160.0f),
	      "0.5x1.0 anamorphic mode recovers the panel too");

	// Degenerate inputs leave the caller's fallback untouched.
	w = 111.0f;
	h = 222.0f;
	CHECK(!dxr::PanelPixelsFromView(0u, 1200u, 0.75f, 0.75f, w, h) &&
	          near_eq(w, 111.0f) && near_eq(h, 222.0f),
	      "zero view width is rejected without touching the outputs");
	CHECK(!dxr::PanelPixelsFromView(1920u, 1200u, 0.0f, 0.75f, w, h) &&
	          near_eq(w, 111.0f) && near_eq(h, 222.0f),
	      "zero view scale is rejected without touching the outputs");

	// End-to-end: the same asset fits differently under the two viewports.
	// A wide asset (2.0 x 1.0) is width-bound on the real panel and would be
	// wrongly height-bound under the atlas aspect -- the user-visible bug.
	const float vhPanel = dxr::AutoFitVHeight(2.0f, 1.0f, 2560.0f, 1600.0f);
	const float vhAtlas = dxr::AutoFitVHeight(2.0f, 1.0f, 3840.0f, 1200.0f);
	CHECK(vhPanel > vhAtlas,
	      "the atlas aspect under-fits a wide asset (the regression guard)");
	CHECK(near_eq(vhPanel, 2.0f / (0.8f * 1.6f)), "panel fit is width-bound");
	CHECK(near_eq(vhAtlas, 1.0f / 0.8f), "atlas fit collapses to height-only");
}

// FitTransition: the viewport-change refit animation.
static void test_fit_transition()
{
	constexpr float EPS = 1e-3f;
	auto near_eq = [&](float a, float b) { return std::fabs(a - b) <= EPS; };

	// Real numbers from a tablet rotation: portrait base 3.67 -> landscape 1.89.
	dxr::FitTransition f;
	CHECK(!f.active(), "a fresh transition is landed, not running");

	float v = -1.0f;
	f.start(3.67f, 1.89f, 0.2f);
	CHECK(f.active(), "start() arms the transition");
	CHECK(near_eq(f.value(), 3.67f), "t=0 sits exactly on the origin");

	CHECK(f.update(0.1f, &v), "half the duration is still running");
	CHECK(near_eq(v, 2.78f), "SmoothStep is symmetric: halfway is the midpoint");

	CHECK(f.update(0.1f, &v), "the landing tick still reports work done");
	CHECK(near_eq(v, 1.89f), "lands exactly on the target");
	CHECK(!f.active() && !f.update(0.1f, &v), "landed, and stays landed");

	// Retarget mid-flight must not snap back to the original origin -- the
	// two-step-settle case a rotation actually produces.
	f.start(3.67f, 1.89f, 0.2f);
	f.update(0.1f, &v);
	const float mid = v;
	f.start(mid, 3.67f, 0.2f);
	CHECK(near_eq(f.value(), mid), "retarget resumes from where it was");
	CHECK(near_eq(f.target(), 3.67f), "retarget adopts the new target");

	// A zero duration is an immediate, valid move rather than a divide-by-zero.
	f.start(1.0f, 2.0f, 0.0f);
	CHECK(!f.active() && near_eq(f.value(), 2.0f),
	      "zero duration lands immediately");

	// The curve matches RigTransition's SmoothStep at the ends and the middle.
	CHECK(near_eq(dxr::FitTransition::curve(0.0f), 0.0f) &&
	          near_eq(dxr::FitTransition::curve(1.0f), 1.0f) &&
	          near_eq(dxr::FitTransition::curve(0.5f), 0.5f),
	      "SmoothStep endpoints and midpoint");
}

// AutoFitCanvas + the aspect gate: the desktop viewport source. Pinned to the
// gaussian-splat numbers that exposed the bug -- a 1280x720 window creation
// size standing in for a square shell tile.
static void test_auto_fit_canvas()
{
	constexpr float EPS = 1e-3f;
	auto near_eq = [&](float a, float b) { return std::fabs(a - b) <= EPS; };

	// The gate. A never-fitted aspect must always count as changed: that is
	// what lands the bootstrap fit when the first canvas finally arrives,
	// with no separate "pending" flag.
	CHECK(dxr::AutoFitAspectChanged(0.0f, 1.0f), "unfitted aspect always refits");
	CHECK(dxr::AutoFitAspectChanged(-1.0f, 1.0f), "negative fitted aspect refits");
	CHECK(!dxr::AutoFitAspectChanged(1.0f, 0.0f), "a degenerate live aspect never refits");
	CHECK(!dxr::AutoFitAspectChanged(1.778f, 1.778f), "an unchanged aspect does not refit");
	CHECK(!dxr::AutoFitAspectChanged(1.778f, 1.7785f),
	      "a resize that keeps proportions does not refit");
	CHECK(dxr::AutoFitAspectChanged(1.778f, 1.0f), "16:9 -> square refits");

	dxr::AutoFitCanvas canvas;
	CHECK(!canvas.Valid() && canvas.Aspect() == 0.0f, "a fresh canvas has not arrived");

	// Before the first locate: the fallback (the app's own client rect) is
	// used, and reported as NOT the runtime canvas.
	float w = 0.0f, h = 0.0f;
	CHECK(!canvas.Viewport(1280.0f, 720.0f, w, h) && near_eq(w, 1280.0f) && near_eq(h, 720.0f),
	      "no canvas yet -> caller's fallback");

	// An empty publish (a locate that resolved no canvas) must not register.
	CHECK(!canvas.PublishFromRaw(0.0f, 0.0f, 0, 0), "an empty raw channel is rejected");
	CHECK(!canvas.Valid(), "a rejected publish leaves the canvas unarrived");

	// Pixels-only (a runtime that filled the rect but not the meters).
	CHECK(canvas.PublishFromRaw(0.0f, 0.0f, 1080, 1080), "pixels accepted when meters are absent");
	CHECK(canvas.Viewport(1280.0f, 720.0f, w, h) && near_eq(w, 1080.0f) && near_eq(h, 1080.0f),
	      "the runtime canvas overrides the fallback");
	CHECK(near_eq(canvas.Aspect(), 1.0f), "square tile reports aspect 1");

	// Meters win over pixels: they are what the rig's m2v divides by.
	CHECK(canvas.PublishFromRaw(0.2f, 0.1f, 1080, 1080), "meters accepted");
	CHECK(near_eq(canvas.Aspect(), 2.0f), "meters win over pixels");

	// A later empty publish must not clobber a good canvas.
	CHECK(!canvas.PublishFromRaw(0.0f, 0.0f, 0, 0), "empty publish still rejected");
	CHECK(near_eq(canvas.Aspect(), 2.0f), "the good canvas survives an empty publish");

	// The regression this exists for, end to end: the butterfly scene fitted
	// against the window creation size vs the square tile it actually renders
	// in. 15% is the difference between framed and overflowing the sides.
	const float vhWindow = dxr::AutoFitVHeight(2.010f, 1.741f, 1280.0f, 720.0f, 0.88f);
	const float vhTile = dxr::AutoFitVHeight(2.010f, 1.741f, 1080.0f, 1080.0f, 0.88f);
	CHECK(near_eq(vhWindow, 1.741f / 0.88f), "the window aspect fits by height");
	CHECK(near_eq(vhTile, 2.010f / 0.88f), "the square tile fits by width");
	CHECK(vhTile > vhWindow * 1.1f, "fitting the wrong viewport oversizes by >10%");
}

static void test_view_params_defaults()
{
    ViewParams vp;
    CHECK(vp.ipdFactor > 0.0f, "ViewParams ipdFactor default should be positive");
}

static void test_window_space_hud_types()
{
    XrHudSwapchain sc;
    CHECK(sc.swapchain == XR_NULL_HANDLE, "XrHudSwapchain default should be null");
    XrCompositionLayerWindowSpaceDXR layer = {};
    layer.type = (XrStructureType)XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_DXR;
    CHECK(layer.width == 0.0f, "window-space layer zero-init");
}

#ifdef _WIN32
static void test_input_state_defaults()
{
    InputState in;
    CHECK(in.hudToggleRequiresShift == true,
          "hudToggleRequiresShift must default true (runtime test-app behavior)");
    CHECK(in.absoluteRenderingModeRequested == -1, "no absolute mode requested at init");
    CHECK(in.transparentBgToggleRequested == false, "no transparency toggle at init");
    CHECK(in.cycleClipRequested == false && in.playPauseRequested == false,
          "clip-playback one-shots clear at init");
    CHECK(in.filePickerRequestRequested == false, "file-picker one-shot clear at init");
}

static void test_session_manager_defaults()
{
    // Instantiating XrSessionManager compiles the whole extension-header chain
    // (XR_DXR_win32_window_binding / display_info / workspace_file_dialog /
    // atlas_capture / mcp_tools). NOTE: deliberately no call into
    // xr_session_common.obj — that TU references OpenXR *loader* symbols
    // (xrEndFrame, …) which the headers-only standalone build cannot link;
    // every real consumer links the loader. Compile coverage comes from the
    // library build itself.
    XrSessionManager xr;
    CHECK(xr.session == XR_NULL_HANDLE, "session null at init");
    CHECK(xr.filePickerInFlight == false && xr.filePickerHasResult == false,
          "file-picker state machine idle at init");
    CHECK(xr.spinSpeed == 0.5f, "MCP-settable spin speed defaults to 0.5");
}

// EndFrame / EndFrameWithWindowSpaceLayers `frameEndNext` (content_bounds.h's
// dxr::ChainContentBounds is the first user — see xr_session_common.h). This
// is a HEADER-ONLY compile check, deliberately not a call or a runtime
// function-pointer read: both functions are defined in xr_session_common.cpp,
// which calls xrEndFrame — an OpenXR *loader* symbol this headers-only
// standalone build does not link (see the NOTE in
// test_session_manager_defaults() above; actually taking &EndFrame would pull
// that .obj out of the static-lib archive and fail to link here). `decltype`
// is an unevaluated context — it type-checks the declaration without
// generating any reference to the symbol — so this proves at compile time
// that `frameEndNext` was added as a trailing `const void*` default argument
// to both signatures, without requiring the loader.
static void test_endframe_frame_end_next_signature()
{
    using EndFrameFn = bool (*)(XrSessionManager&, XrTime,
                                 const XrCompositionLayerProjectionView*,
                                 uint32_t, XrCompositionLayerFlags,
                                 const void*, const void*);
    static_assert(std::is_same<decltype(&EndFrame), EndFrameFn>::value,
                  "EndFrame must end with frameEndNext (const void*), chained onto "
                  "XrFrameEndInfo::next");

    using EndFrameWindowSpaceFn = bool (*)(
        XrSessionManager&, XrTime, const XrCompositionLayerProjectionView*,
        float, float, float, float, float, uint32_t,
        const void*, uint32_t,
        int32_t, int32_t, int32_t, int32_t,
        bool, XrCompositionLayerFlags, const void*,
        const XrCompositionLayerBaseHeader* const*, uint32_t,
        const void*);
    static_assert(std::is_same<decltype(&EndFrameWithWindowSpaceLayers), EndFrameWindowSpaceFn>::value,
                  "EndFrameWithWindowSpaceLayers must end with frameEndNext (const void*), "
                  "chained onto XrFrameEndInfo::next");
}

static void test_win32_link_closure()
{
    // Function-POINTER references (no calls — the runners have no GPU/session):
    // each pulls its TU out of the static lib, forcing the d3d11 / dxgi /
    // d3dcompiler / d2d1 / dwrite link closure and the user32/gdi32 Win32
    // surface to resolve at link time. volatile defeats "always non-null"
    // constant folding / warnings.
    volatile auto pD3D = &InitializeD3D11;          // d3d11_renderer → d3d11/dxgi/d3dcompiler
    volatile auto pHud = &RenderButtonStandalone;   // hud_renderer → text_overlay
    volatile auto pTxt = &InitializeTextOverlay;    // text_overlay → d2d1/dwrite
    volatile auto pIn  = &UpdateInputState;         // input_handler → user32
    volatile auto pWin = &CreateAppWindow;          // window_manager → user32/gdi32
    CHECK(pD3D && pHud && pTxt && pIn && pWin, "Win32 scaffolding TUs linked");
}
#endif

#ifdef __APPLE__
static void test_macos_hud_rasterize()
{
    // CPU CoreText rasterization — runner-safe, no GPU.
    HudRendererMacOS hud;
    if (!InitializeHudRenderer(hud, 256, 128)) {
        CHECK(false, "InitializeHudRenderer (macOS) failed");
        return;
    }
    uint32_t pitch = 0;
    const void* px = RenderHudAndMap(hud, &pitch,
        L"Session: SMOKE", L"Mode: 3D", L"60 fps", L"", L"");
    CHECK(px != nullptr && pitch >= 256 * 4, "macOS HUD rasterization produced pixels");
    UnmapHud(hud);
    CleanupHudRenderer(hud);
}

static void test_stb_symbols_link()
{
    // Pull both STB implementation TUs out of the static lib: the lib must own
    // exactly one read- and one write-implementation on Apple (duplicate-symbol
    // canary for stb_image_impl_macos.cpp + atlas_capture_macos.mm).
    int w = 0, h = 0, comp = 0;
    unsigned char bogus[4] = {0, 1, 2, 3};
    stbi_uc* img = stbi_load_from_memory(bogus, 4, &w, &h, &comp, 4);
    CHECK(img == nullptr, "bogus buffer must not decode");
    CHECK(stbi_failure_reason() != nullptr, "stb_image failure reason wired");
    volatile auto pWrite = &stbi_write_png;
    CHECK(pWrite != nullptr, "stb_image_write linked");
}
#endif

// Smooth 2D<->3D mode-switch sequencer (mode_switch.h). Pure, deterministic.
static void test_mode_switch()
{
    const float DT = 0.06f; // 3 frames spans the default 0.18 s ramp.

    // --- 3D -> 2D: ramp disparity to 0 FIRST, then fire on landing. ---
    {
        dxr::ModeSwitch ms;
        ms.configure(0.18f, dxr::ModeSwitchEasing::SmoothStep);
        ms.request(/*targetMode*/ 0, /*targetVC*/ 1, /*curMode*/ 1, /*curVC*/ 2,
                   /*curIpd*/ 1.0f, /*steadyIpd*/ 1.0f);
        CHECK(ms.active(), "3D->2D should be active after request");

        float ipd = 1.0f, prev = 2.0f;
        bool fired = false;
        uint32_t firedMode = 999, fireCount = 0;
        for (int i = 0; i < 4; i++) {
            bool fire = false;
            uint32_t mode = 999;
            ms.update(DT, &ipd, &fire, &mode);
            CHECK(ipd <= prev + 1e-4f, "3D->2D disparity must be monotonically non-increasing");
            prev = ipd;
            if (fire) { fired = true; firedMode = mode; fireCount++; }
        }
        CHECK(fired && firedMode == 0, "3D->2D must fire the 2D mode exactly once, on landing");
        CHECK(fireCount == 1, "3D->2D fire must be edge (one frame only)");
        CHECK(ipd < 1e-3f, "3D->2D must land at zero disparity");
        CHECK(!ms.active(), "3D->2D should be idle after landing");
    }

    // --- 2D -> 3D: fire IMMEDIATELY (flat first frame), then ramp up. ---
    {
        dxr::ModeSwitch ms;
        ms.configure(0.18f);
        ms.request(/*target*/ 1, /*tVC*/ 2, /*curMode*/ 0, /*curVC*/ 1,
                   /*curIpd*/ 0.0f, /*steady*/ 1.0f);

        float ipd = -1.0f, prev = -1.0f;
        uint32_t fireCount = 0, firedMode = 999;
        int fireFrame = -1;
        for (int i = 0; i < 4; i++) {
            bool fire = false;
            uint32_t mode = 999;
            ms.update(DT, &ipd, &fire, &mode);
            if (fire) { fireCount++; firedMode = mode; if (fireFrame < 0) fireFrame = i; }
            CHECK(ipd >= prev - 1e-4f, "2D->3D disparity must be monotonically non-decreasing");
            prev = ipd;
        }
        CHECK(fireCount == 1 && fireFrame == 0, "2D->3D must fire once, on the FIRST frame");
        CHECK(firedMode == 1, "2D->3D must fire the requested 3D mode");
        CHECK(ipd > 0.99f, "2D->3D must ramp up to the steady disparity");
        CHECK(!ms.active(), "2D->3D should be idle after landing");
    }

    // --- Interruption: reverse a NOT-YET-FIRED ->2D back to 3D. Must never
    //     fire a mode switch and must restore steady disparity. ---
    {
        dxr::ModeSwitch ms;
        ms.configure(0.18f);
        ms.request(0, 1, 1, 2, 1.0f, 1.0f); // start 3D->2D

        float ipd = 1.0f;
        bool everFired = false;
        // Advance partway (not enough to land/fire).
        for (int i = 0; i < 2; i++) {
            bool fire = false;
            ms.update(DT, &ipd, &fire, nullptr);
            if (fire) everFired = true;
        }
        CHECK(!everFired && ipd > 0.0f && ipd < 1.0f, "mid ramp-down: not fired, partially flat");

        // Runtime is still in 3D (the 2D switch never fired) → reverse to 3D.
        ms.request(/*target*/ 1, /*tVC*/ 2, /*curMode*/ 1, /*curVC*/ 2, ipd, 1.0f);
        for (int i = 0; i < 4; i++) {
            bool fire = false;
            ms.update(DT, &ipd, &fire, nullptr);
            if (fire) everFired = true;
        }
        CHECK(!everFired, "reversing an un-fired ->2D must NEVER issue a mode switch");
        CHECK(ipd > 0.99f, "reversal must restore steady disparity");
        CHECK(!ms.active(), "reversal should settle to idle");
    }

    // --- Instant (duration 0): fire + reach endpoint on the first update. ---
    {
        dxr::ModeSwitch ms;
        ms.configure(0.0f);
        ms.request(1, 2, 0, 1, 0.0f, 1.0f); // 2D->3D instant
        float ipd = -1.0f;
        bool fire = false;
        uint32_t mode = 999;
        ms.update(0.016f, &ipd, &fire, &mode);
        CHECK(fire && mode == 1 && ipd > 0.99f && !ms.active(),
              "instant 2D->3D must fire and reach steady in one frame");

        ms.request(0, 1, 1, 2, 1.0f, 1.0f); // 3D->2D instant
        ms.update(0.016f, &ipd, &fire, &mode);
        CHECK(fire && mode == 0 && ipd < 1e-3f && !ms.active(),
              "instant 3D->2D must fire and flatten in one frame");
    }

    // --- Same-dimensionality 3D->3D (real mode change): fire once, no flatten. ---
    {
        dxr::ModeSwitch ms;
        ms.configure(0.18f);
        ms.request(/*target*/ 2, /*tVC*/ 2, /*curMode*/ 1, /*curVC*/ 2, 1.0f, 1.0f);
        float ipd = 0.0f;
        uint32_t fireCount = 0, firedMode = 999;
        for (int i = 0; i < 4; i++) {
            bool fire = false;
            uint32_t mode = 999;
            ms.update(DT, &ipd, &fire, &mode);
            if (fire) { fireCount++; firedMode = mode; }
            CHECK(ipd > 0.99f, "3D->3D must keep full disparity throughout (no flatten)");
        }
        CHECK(fireCount == 1 && firedMode == 2, "3D->3D must fire the new mode once");
    }
}

// Off-axis projection self-test: display-rig math, plus display<->camera rig
// equivalence (frustum + view matrix match at any perspective, round-trip
// identity). Pure, GPU-free.
static void test_rig_math()
{
    CHECK(dxr_display3d_selftest() == 0, "dxr_display3d_selftest (incl. rig equivalence) reported failures");
}

// dxr::ResolveClipPlanes / ChainRearDepthBudget / RearDepthBudgetStateName
// (clip_policy.h, #38). Pure, deterministic; the math mirrors
// dxr_display3d_compute_view's ZDP-relative near/far derivation.
static void test_clip_policy()
{
    const float EPS = 1e-4f;
    auto near_eq = [&](float a, float b) { return a > b - EPS && a < b + EPS; };

    const float ez = 0.65f;
    const float vH = 0.3f;

    // No budget (older runtime / extension not enabled): transparent +
    // standalone reproduces today's hard ZDP clip bit-for-bit.
    {
        dxr::ClipPlanes clip = dxr::ResolveClipPlanes(ez, vH, nullptr, /*transparent=*/true, /*standalone=*/true);
        CHECK(near_eq(clip.farOffsetVH, 0.0f), "no budget + transparent + standalone -> farOffsetVH 0");
        CHECK(near_eq(clip.near_z, ez - vH), "near_z = ez - vH");
        CHECK(near_eq(clip.far_z, ez), "far_z = ez when farOffsetVH is 0");
        CHECK(near_eq(clip.clipFar, clip.far_z), "clipFar must equal far_z when clipping");
    }

    // No budget, but NOT (transparent && standalone): unrestricted, no cull.
    {
        dxr::ClipPlanes clipOpaque = dxr::ResolveClipPlanes(ez, vH, nullptr, /*transparent=*/false, /*standalone=*/true);
        CHECK(near_eq(clipOpaque.farOffsetVH, 1000.0f), "opaque session -> unrestricted farOffsetVH");
        CHECK(clipOpaque.clipFar == 0.0f, "opaque session must never cull");

        dxr::ClipPlanes clipWorkspace = dxr::ResolveClipPlanes(ez, vH, nullptr, /*transparent=*/true, /*standalone=*/false);
        CHECK(near_eq(clipWorkspace.farOffsetVH, 1000.0f), "under-workspace session -> unrestricted farOffsetVH");
        CHECK(clipWorkspace.clipFar == 0.0f, "under-workspace session must never cull");
    }

    // A budget overrides the fallback rule outright, including for a
    // standalone transparent session (the runtime decided it can open up).
    {
        XrRearDepthBudgetDXR budget{};
        budget.type = (XrStructureType)XR_TYPE_REAR_DEPTH_BUDGET_DXR;
        budget.farOffsetVH = 4.0f;
        budget.state = XR_REAR_DEPTH_BUDGET_STATE_OPEN_DXR;

        dxr::ClipPlanes clip = dxr::ResolveClipPlanes(ez, vH, &budget, /*transparent=*/true, /*standalone=*/true);
        CHECK(near_eq(clip.farOffsetVH, 4.0f), "budget's farOffsetVH must be used as-is (no smoothing)");
        CHECK(near_eq(clip.far_z, ez + 4.0f * vH), "far_z = ez + farOffsetVH * vH");
        CHECK(clip.clipFar != 0.0f, "still clips (farOffsetVH < 1000) while the budget is finite");
    }

    // farOffsetVH >= 1000 (unrestricted) must disable the hard clip even with
    // a budget present.
    {
        XrRearDepthBudgetDXR budget{};
        budget.type = (XrStructureType)XR_TYPE_REAR_DEPTH_BUDGET_DXR;
        budget.farOffsetVH = 1000.0f;
        budget.state = XR_REAR_DEPTH_BUDGET_STATE_UNRESTRICTED_WORKSPACE_DXR;

        dxr::ClipPlanes clip = dxr::ResolveClipPlanes(ez, vH, &budget, /*transparent=*/true, /*standalone=*/false);
        CHECK(clip.clipFar == 0.0f, "farOffsetVH >= 1000 must never cull");
    }

    // The runtime's session-level transparent flag is set at xrCreateSession and
    // cannot follow a per-frame Ctrl+T toggle, so a budget of 0 (busy desktop)
    // can arrive while the app is drawing OPAQUE. The app's own `transparent`
    // must win: an opaque frame is never clipped, whatever the budget says.
    {
        XrRearDepthBudgetDXR budget{};
        budget.type = (XrStructureType)XR_TYPE_REAR_DEPTH_BUDGET_DXR;
        budget.farOffsetVH = 0.0f;
        budget.state = XR_REAR_DEPTH_BUDGET_STATE_CLIPPED_BUSY_BACKGROUND_DXR;

        dxr::ClipPlanes clip = dxr::ResolveClipPlanes(ez, vH, &budget, /*transparent=*/false, /*standalone=*/true);
        CHECK(near_eq(clip.farOffsetVH, 1000.0f), "opaque frame ignores a clipping budget -> unrestricted");
        CHECK(near_eq(clip.far_z, ez + 1000.0f * vH), "opaque frame keeps the unrestricted far plane");
        CHECK(clip.clipFar == 0.0f, "opaque frame must never cull, even with a budget of 0");
    }

    // Near-degenerate eye distance: clipFar must not fire at/behind the near
    // plane (the demos' existing ez > 0.2 guard), even while clipping is
    // otherwise active.
    {
        dxr::ClipPlanes clip = dxr::ResolveClipPlanes(/*ez=*/0.1f, vH, nullptr, /*transparent=*/true, /*standalone=*/true);
        CHECK(clip.clipFar == 0.0f, "clipFar must not fire when ez <= 0.2");
        CHECK(clip.near_z >= 1.0e-4f, "near_z must stay positive");
    }

    // Degenerate near/far ordering never inverts: far_z is always > near_z.
    {
        dxr::ClipPlanes clip = dxr::ResolveClipPlanes(/*ez=*/1.0e-5f, /*vH=*/1.0f, nullptr, false, false);
        CHECK(clip.far_z > clip.near_z, "far_z must stay strictly past near_z even at a degenerate eye distance");
    }

    // ChainRearDepthBudget: links onto XrViewState::next, preserves an
    // existing chain, and stamps the correct type.
    {
        int sentinelChain = 0;
        XrViewState vs{XR_TYPE_VIEW_STATE, &sentinelChain, 0};
        XrRearDepthBudgetDXR out;
        CHECK(dxr::ChainRearDepthBudget(vs, out), "ChainRearDepthBudget must succeed");
        CHECK(out.type == XR_TYPE_REAR_DEPTH_BUDGET_DXR, "chained struct must carry the extension's type");
        CHECK(vs.next == &out, "XrViewState::next must point at the chained struct");
        CHECK(out.next == &sentinelChain, "the chained struct must preserve the prior chain");
    }

    // RearDepthBudgetStateName: every enumerator gets a distinct, non-null name.
    {
        CHECK(std::string(dxr::RearDepthBudgetStateName(XR_REAR_DEPTH_BUDGET_STATE_UNRESTRICTED_OPAQUE_DXR)) ==
                  "UnrestrictedOpaque",
              "state name: UnrestrictedOpaque");
        CHECK(std::string(dxr::RearDepthBudgetStateName(XR_REAR_DEPTH_BUDGET_STATE_OPEN_DXR)) == "Open",
              "state name: Open");
        CHECK(std::string(dxr::RearDepthBudgetStateName(XR_REAR_DEPTH_BUDGET_STATE_FORCED_DXR)) == "Forced",
              "state name: Forced");
        CHECK(std::string(dxr::RearDepthBudgetStateName(XR_REAR_DEPTH_BUDGET_STATE_MAX_ENUM_DXR)) == "Unknown",
              "state name: unrecognized value falls back to Unknown");
    }
}

// dxr::ProjectAabbToCanvasBounds / ChainContentBounds (content_bounds.h,
// XR_DXR_depth_budget v2, rear-depth-budget brief §5.1). Pure, deterministic.
//
// Both eyes below share one symmetric perspective projection with fovy=90deg
// (cot(45deg)=1) and aspect=1, near=0.1/far=100 -- chosen so the projection's
// column-major entries are m[0]=m[5]=1, m[10]=(f+n)/(n-f), m[11]=-1,
// m[14]=2fn/(n-f), everything else 0, and every corner's projected w reduces
// to exactly -z. That makes the expected canvas rect hand-computable in exact
// fractions (see the comments at each call site) rather than needing a matrix
// library in the test itself.
static void test_content_bounds()
{
    const float EPS = 1e-3f;
    auto near_eq = [&](float a, float b) { return std::fabs(a - b) <= EPS; };

    // Shared symmetric perspective projection (see banner comment above).
    // m[11] = -1 makes projected w == -z for any point with w_in == 1.
    float eye1[16] = {0};
    eye1[0] = 1.0f;
    eye1[5] = 1.0f;
    eye1[10] = (100.0f + 0.1f) / (0.1f - 100.0f);
    eye1[11] = -1.0f;
    eye1[14] = (2.0f * 0.1f * 100.0f) / (0.1f - 100.0f);

    // A unit cube in front of the camera, x/y in [-0.5,0.5], z in [-2.5,-1.5]
    // (camera looks down -Z, so this is "1 to 2.5 units in front").
    const float aabbMin[3] = {-0.5f, -0.5f, -2.5f};
    const float aabbMax[3] = {0.5f, 0.5f, -1.5f};

    // --- Single eye: hand-computed centred rect. ---
    // Near corners (z=-1.5, w=1.5) dominate: ndcX = +-0.5/1.5 = +-1/3, same
    // for ndcY. u = (ndcX+1)/2 -> [1/3, 2/3]; v = (1-ndcY)/2 -> [1/3, 2/3]
    // (v DOWN, but the cube is symmetric in y so the range is the same).
    {
        const float* eyes[1] = {eye1};
        XrRect2Df rect{};
        bool ok = dxr::ProjectAabbToCanvasBounds(aabbMin, aabbMax, eyes, 1, &rect);
        CHECK(ok, "single-eye projection of an in-frustum cube must succeed");
        CHECK(near_eq(rect.offset.x, 1.0f / 3.0f), "single-eye offset.x == 1/3");
        CHECK(near_eq(rect.offset.y, 1.0f / 3.0f), "single-eye offset.y == 1/3");
        CHECK(near_eq(rect.extent.width, 1.0f / 3.0f), "single-eye extent.width == 1/3");
        CHECK(near_eq(rect.extent.height, 1.0f / 3.0f), "single-eye extent.height == 1/3");
    }

    // --- Two eyes, second offset in X (an off-axis/Kooima-style lens shift,
    //     m[12] += -0.4): union must be strictly WIDER than either eye alone.
    //     Hand-computed union: ndcX in [-0.6, 1/3] -> u in [0.2, 2/3];
    //     eye2 never touches Y, so the v range is unchanged.
    {
        float eye2[16];
        std::memcpy(eye2, eye1, sizeof(eye2));
        eye2[12] = -0.4f;

        const float* eyes[2] = {eye1, eye2};
        XrRect2Df rect{};
        bool ok = dxr::ProjectAabbToCanvasBounds(aabbMin, aabbMax, eyes, 2, &rect);
        CHECK(ok, "two-eye projection of an in-frustum cube must succeed");
        CHECK(near_eq(rect.offset.x, 0.2f), "union offset.x == 0.2 (widened left)");
        CHECK(near_eq(rect.extent.width, 2.0f / 3.0f - 0.2f), "union extent.width matches hand calc");
        CHECK(rect.extent.width > 1.0f / 3.0f + EPS,
              "union of two eyes must be strictly wider than a single eye");
        CHECK(near_eq(rect.offset.y, 1.0f / 3.0f) && near_eq(rect.extent.height, 1.0f / 3.0f),
              "the Y range is untouched by an X-only eye offset");
    }

    // --- A corner behind the eye (w <= 0): must fail closed to the whole
    //     canvas, not a partial/garbage rect. ---
    {
        const float behindMin[3] = {-0.5f, -0.5f, -1.0f};
        const float behindMax[3] = {0.5f, 0.5f, 0.5f}; // z=+0.5 is behind the camera
        const float* eyes[1] = {eye1};
        XrRect2Df rect{};
        rect.offset.x = 42.0f; // sentinel: must be overwritten even on failure
        bool ok = dxr::ProjectAabbToCanvasBounds(behindMin, behindMax, eyes, 1, &rect);
        CHECK(!ok, "a corner behind the eye must return false");
        CHECK(near_eq(rect.offset.x, 0.0f) && near_eq(rect.offset.y, 0.0f) &&
                  near_eq(rect.extent.width, 1.0f) && near_eq(rect.extent.height, 1.0f),
              "failure must write the whole canvas, not leave garbage");
    }

    // --- Clamping: a box far wider than the frustum clamps to the full
    //     canvas but still reports success (every corner has w > 0). ---
    {
        const float wideMin[3] = {-10.0f, -10.0f, -2.5f};
        const float wideMax[3] = {10.0f, 10.0f, -1.5f};
        const float* eyes[1] = {eye1};
        XrRect2Df rect{};
        bool ok = dxr::ProjectAabbToCanvasBounds(wideMin, wideMax, eyes, 1, &rect);
        CHECK(ok, "an out-of-frustum-but-in-front box still succeeds (clamped, not failed)");
        CHECK(near_eq(rect.offset.x, 0.0f) && near_eq(rect.offset.y, 0.0f) &&
                  near_eq(rect.extent.width, 1.0f) && near_eq(rect.extent.height, 1.0f),
              "an oversized box clamps to the whole canvas");
    }

    // --- Degenerate/null input: fails closed to the whole canvas too. ---
    {
        XrRect2Df rect{};
        CHECK(!dxr::ProjectAabbToCanvasBounds(aabbMin, aabbMax, nullptr, 0, &rect),
              "zero eyeCount / null viewProj must fail");
        CHECK(near_eq(rect.extent.width, 1.0f) && near_eq(rect.extent.height, 1.0f),
              "degenerate input still writes the whole canvas");
    }

    // --- ChainContentBounds: links onto XrFrameEndInfo::next, preserves an
    //     existing chain, and copies bounds + margin verbatim. ---
    {
        int sentinelChain = 0;
        XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO, &sentinelChain, 0, XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 0, nullptr};
        XrRect2Df bounds{};
        bounds.offset.x = 0.25f;
        bounds.offset.y = 0.1f;
        bounds.extent.width = 0.5f;
        bounds.extent.height = 0.6f;

        XrContentBoundsDXR out;
        CHECK(dxr::ChainContentBounds(fei, out, bounds, 0.02f), "ChainContentBounds must succeed");
        CHECK(out.type == (XrStructureType)XR_TYPE_CONTENT_BOUNDS_DXR,
              "chained struct must carry the extension's type");
        CHECK(fei.next == &out, "XrFrameEndInfo::next must point at the chained struct");
        CHECK(out.next == &sentinelChain, "the chained struct must preserve the prior chain");
        CHECK(near_eq(out.bounds.offset.x, 0.25f) && near_eq(out.bounds.offset.y, 0.1f) &&
                  near_eq(out.bounds.extent.width, 0.5f) && near_eq(out.bounds.extent.height, 0.6f),
              "bounds must be copied verbatim");
        CHECK(near_eq(out.marginNormalized, 0.02f), "marginNormalized must be copied verbatim");
    }

    // --- RebaseZoneBoundsToWindow: a 3D zone occupying the bottom half of a
    //     100x200 window (zoneRectPx = {0,100} / {100,100}). ---
    {
        XrRect2Di zoneRectPx{};
        zoneRectPx.offset.x = 0;
        zoneRectPx.offset.y = 100;
        zoneRectPx.extent.width = 100;
        zoneRectPx.extent.height = 100;

        // zone-normalised {0.25,0.5 / 0.5,0.5} -> window {0.25,0.75 / 0.5,0.25}.
        {
            XrRect2Df zoneNorm{};
            zoneNorm.offset.x = 0.25f;
            zoneNorm.offset.y = 0.5f;
            zoneNorm.extent.width = 0.5f;
            zoneNorm.extent.height = 0.5f;

            XrRect2Df rect{};
            bool ok = dxr::RebaseZoneBoundsToWindow(zoneNorm, zoneRectPx, 100, 200, &rect);
            CHECK(ok, "RebaseZoneBoundsToWindow must succeed for a well-formed zone");
            CHECK(near_eq(rect.offset.x, 0.25f), "rebased offset.x == 0.25");
            CHECK(near_eq(rect.offset.y, 0.75f), "rebased offset.y == 0.75 (bottom-half zone)");
            CHECK(near_eq(rect.extent.width, 0.5f), "rebased extent.width == 0.5");
            CHECK(near_eq(rect.extent.height, 0.25f), "rebased extent.height == 0.25 (half of the zone's half)");
        }

        // v0 = -0.2 clamps to 0 BEFORE rebase: the result must land exactly
        // on the zone's own top edge (window y=0.5), never above it.
        {
            XrRect2Df zoneNorm{};
            zoneNorm.offset.x = 0.25f;
            zoneNorm.offset.y = -0.2f;
            zoneNorm.extent.width = 0.5f;
            zoneNorm.extent.height = 0.7f; // spans [-0.2, 0.5] pre-clamp

            XrRect2Df rect{};
            bool ok = dxr::RebaseZoneBoundsToWindow(zoneNorm, zoneRectPx, 100, 200, &rect);
            CHECK(ok, "RebaseZoneBoundsToWindow must succeed with an out-of-range-but-clampable input");
            CHECK(near_eq(rect.offset.y, 0.5f),
                  "a v0 below 0 must clamp to the zone's own top edge, never reach above it");
            CHECK(near_eq(rect.offset.y + rect.extent.height, 0.75f),
                  "the clamped rect's bottom edge is unaffected");
        }

        // Degenerate zone (zero-area zoneRectPx) -> whole window, false.
        {
            XrRect2Di degenerateZone{};
            degenerateZone.offset.x = 0;
            degenerateZone.offset.y = 100;
            degenerateZone.extent.width = 0;
            degenerateZone.extent.height = 100;

            XrRect2Df zoneNorm{};
            zoneNorm.offset.x = 0.25f;
            zoneNorm.offset.y = 0.5f;
            zoneNorm.extent.width = 0.5f;
            zoneNorm.extent.height = 0.5f;

            XrRect2Df rect{};
            rect.offset.x = 42.0f; // sentinel: must be overwritten
            bool ok = dxr::RebaseZoneBoundsToWindow(zoneNorm, degenerateZone, 100, 200, &rect);
            CHECK(!ok, "a zero-area zoneRectPx must return false");
            CHECK(near_eq(rect.offset.x, 0.0f) && near_eq(rect.offset.y, 0.0f) &&
                      near_eq(rect.extent.width, 1.0f) && near_eq(rect.extent.height, 1.0f),
                  "degenerate zone rebase writes the whole window, not garbage");
        }

        // Degenerate window dims -> whole window, false.
        {
            XrRect2Df zoneNorm{};
            zoneNorm.extent.width = 0.5f;
            zoneNorm.extent.height = 0.5f;
            XrRect2Df rect{};
            bool ok = dxr::RebaseZoneBoundsToWindow(zoneNorm, zoneRectPx, 0, 200, &rect);
            CHECK(!ok, "a zero window width must return false");
            CHECK(near_eq(rect.extent.width, 1.0f) && near_eq(rect.extent.height, 1.0f),
                  "zero-window-dim rebase writes the whole window");
        }
    }

    // --- ProjectAabbToWindowBounds: one-call variant must equal
    //     project-then-rebase performed manually with the same inputs. ---
    {
        XrRect2Di zoneRectPx{};
        zoneRectPx.offset.x = 0;
        zoneRectPx.offset.y = 100;
        zoneRectPx.extent.width = 100;
        zoneRectPx.extent.height = 100;

        const float* eyes[1] = {eye1};

        XrRect2Df projected{};
        bool projectedOk = dxr::ProjectAabbToCanvasBounds(aabbMin, aabbMax, eyes, 1, &projected);
        XrRect2Df expected{};
        bool expectedOk = dxr::RebaseZoneBoundsToWindow(projected, zoneRectPx, 100, 200, &expected);

        XrRect2Df combined{};
        bool combinedOk =
            dxr::ProjectAabbToWindowBounds(aabbMin, aabbMax, eyes, 1, zoneRectPx, 100, 200, &combined);

        CHECK(projectedOk && expectedOk, "manual project-then-rebase reference path must succeed");
        CHECK(combinedOk == expectedOk, "ProjectAabbToWindowBounds success must match the manual two-step path");
        CHECK(near_eq(combined.offset.x, expected.offset.x) && near_eq(combined.offset.y, expected.offset.y) &&
                  near_eq(combined.extent.width, expected.extent.width) &&
                  near_eq(combined.extent.height, expected.extent.height),
              "ProjectAabbToWindowBounds must equal project-then-rebase performed manually");
    }

    // --- ProjectAabbToWindowBounds: zoneRectPx with <= 0 extent means "the
    //     zone is the whole window" -> no rebase, output is the raw
    //     projected rect. ---
    {
        XrRect2Di wholeWindowZone{}; // extent defaults to {0,0}
        const float* eyes[1] = {eye1};

        XrRect2Df projected{};
        bool projectedOk = dxr::ProjectAabbToCanvasBounds(aabbMin, aabbMax, eyes, 1, &projected);

        XrRect2Df rect{};
        bool ok = dxr::ProjectAabbToWindowBounds(aabbMin, aabbMax, eyes, 1, wholeWindowZone, 100, 200, &rect);
        CHECK(ok == projectedOk, "a zero-extent zoneRectPx must skip the rebase, not fail it");
        CHECK(near_eq(rect.offset.x, projected.offset.x) && near_eq(rect.offset.y, projected.offset.y) &&
                  near_eq(rect.extent.width, projected.extent.width) &&
                  near_eq(rect.extent.height, projected.extent.height),
              "a zero-extent zoneRectPx must pass the projected rect through unchanged");
    }
}

int main()
{
    test_capture_numbering();
    test_mip_chain();
    test_auto_fit();
    test_panel_px_from_view();
    test_fit_transition();
    test_auto_fit_canvas();
    test_view_params_defaults();
    test_window_space_hud_types();
    test_mode_switch();
    test_rig_math();
    test_clip_policy();
    test_content_bounds();
#ifdef _WIN32
    test_input_state_defaults();
    test_session_manager_defaults();
    test_endframe_frame_end_next_signature();
#endif
#ifdef __APPLE__
    test_macos_hud_rasterize();
    test_stb_symbols_link();
#endif

    if (g_failures != 0) {
        std::fprintf(stderr, "displayxr_common_smoke: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("displayxr_common_smoke: all checks passed\n");
    return 0;
}

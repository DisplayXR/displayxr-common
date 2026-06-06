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

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "atlas_capture.h"
#include "mip_chain.h"
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

static void test_view_params_defaults()
{
    ViewParams vp;
    CHECK(vp.ipdFactor > 0.0f, "ViewParams ipdFactor default should be positive");
}

static void test_window_space_hud_types()
{
    XrHudSwapchain sc;
    CHECK(sc.swapchain == XR_NULL_HANDLE, "XrHudSwapchain default should be null");
    XrCompositionLayerWindowSpaceEXT layer = {};
    layer.type = (XrStructureType)XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_EXT;
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
    // (XR_EXT_win32_window_binding / display_info / workspace_file_dialog /
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

int main()
{
    test_capture_numbering();
    test_mip_chain();
    test_view_params_defaults();
    test_window_space_hud_types();
#ifdef _WIN32
    test_input_state_defaults();
    test_session_manager_defaults();
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

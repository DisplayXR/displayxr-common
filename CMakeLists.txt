cmake_minimum_required(VERSION 3.21)

project(displayxr_common
    VERSION 0.3.0
    DESCRIPTION "Shared off-axis Kooima projection math + C++ app scaffolding for DisplayXR 3D displays"
    LANGUAGES C CXX)

# Objective-C++ for the macOS scaffolding sources (.mm). Safe if the consumer
# already enabled it; required for the standalone (CI) build on Apple.
if(APPLE)
    enable_language(OBJCXX)
endif()

option(DISPLAYXR_COMMON_BUILD_TESTS "Build the displayxr-common selftest harness" ON)

include(FetchContent)

# ---------------------------------------------------------------------------
# OpenXR headers
#
# displayxr::math (the app-side FOV+matrix layer) is typed in OpenXR types
# (XrVector3f / XrQuaternionf / XrFovf / XrPosef). It needs only the *headers*,
# not the loader, so pull OpenXR-SDK with the loader / layers / tests disabled
# and link its header-only interface target. Every real consumer (runtime test
# apps, demos, Unreal, Unity) already builds against OpenXR, so this avoids the
# type-redefinition that a vendored copy of those structs would risk.
#
# (Deferred — see README "Roadmap": a type-neutral core would drop even this
# header dep and let the runtime's FOV-only xrt-typed layer share the source.)
# ---------------------------------------------------------------------------
# OpenXR headers for the Xr* types. Three cases, in priority order:
#
#  1. The consumer already provides an OpenXR target (OpenXR::headers or
#     OpenXR::openxr_loader) — reuse it.
#  2. Nothing yet, AND the consumer find_package(OpenXR)s LATER (e.g. a demo
#     whose apps set up OpenXR in subdirectories after add_subdirectory(common)).
#     We must NOT create OpenXR::* targets here: OpenXR-SDK's export set defines
#     OpenXR::headers, and the consumer's later find_package then fails with
#     "some (but not all) targets in this export set were already defined". So
#     we POPULATE the SDK source (its openxr.h is committed) WITHOUT running its
#     CMake, and expose the include dir via a privately-named interface target.
#  3. The truly standalone build (this repo's own CI selftest) also lands in (2)
#     and just uses the populated headers.
if(TARGET OpenXR::headers)
    set(DISPLAYXR_MATH_OPENXR OpenXR::headers)
elseif(TARGET OpenXR::openxr_loader)
    set(DISPLAYXR_MATH_OPENXR OpenXR::openxr_loader)
else()
    FetchContent_Declare(
        openxr_sdk_headers
        GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
        GIT_TAG release-1.1.43
        GIT_SHALLOW TRUE
    )
    FetchContent_GetProperties(openxr_sdk_headers)
    if(NOT openxr_sdk_headers_POPULATED)
        FetchContent_Populate(openxr_sdk_headers)
    endif()
    add_library(displayxr_openxr_headers INTERFACE)
    target_include_directories(displayxr_openxr_headers INTERFACE
        "${openxr_sdk_headers_SOURCE_DIR}/include")
    set(DISPLAYXR_MATH_OPENXR displayxr_openxr_headers)
endif()

# ---------------------------------------------------------------------------
# displayxr::math — pure-C Kooima off-axis projection (FOV + matrices)
#
# Members (reconciled superset, seeded from displayxr-runtime test_apps/common):
#   display3d_view.{c,h}   display-centric rig + projection + center-view + selftest
#   camera3d_view.{c,h}    camera-centric rig
#   projection_depth.h     GL [-1,1] -> [0,1] clip-depth remap for D3D/VK/Metal
#
# NOTE: leia_math.h is intentionally NOT here — it is a Windows-only
# (DirectXMath, C++) Leia SDK reference example, not portable math; it stays
# app-local. The runtime's FOV-only xrt-typed ports (m_camera3d_view /
# m_display3d_view / m_multiview) are a separate layer, deferred (see README).
# ---------------------------------------------------------------------------
add_library(displayxr_math STATIC
    include/display3d_view.c
    include/camera3d_view.c
)
add_library(displayxr::math ALIAS displayxr_math)

target_include_directories(displayxr_math PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(displayxr_math PUBLIC ${DISPLAYXR_MATH_OPENXR})
set_target_properties(displayxr_math PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# DisplayXR OpenXR extension headers (XR_EXT_display_info.h,
# XR_EXT_win32_window_binding.h, XR_EXT_workspace_file_dialog.h,
# XR_EXT_atlas_capture.h, XR_EXT_mcp_tools.h, …) — needed by
# xr_session_common / xr_window_space_hud in displayxr::common. These are NOT
# in the Khronos OpenXR-SDK; their source of truth is the runtime's
# src/external/openxr_includes, auto-published to
# github.com/DisplayXR/displayxr-extensions. Mirrors the OpenXR guard above,
# in priority order:
#
#  1. The consumer sets DISPLAYXR_EXTENSIONS_INCLUDE_DIR (the directory
#     CONTAINING openxr/XR_EXT_*.h) before bringing in this project —
#     preferred: the lib then compiles against the consumer's exact header
#     snapshot, and when that dir also carries the full Khronos set (it does
#     for every current consumer) it wins the include order over any fetched
#     SDK copy, so lib TUs and app TUs see the SAME openxr.h.
#  2. Standalone (this repo's own CI): fetch displayxr-extensions at a pinned
#     commit (the repo is untagged). Privately-named interface target — never
#     creates OpenXR::* names, same reasoning as the guard above.
# ---------------------------------------------------------------------------
if(DISPLAYXR_EXTENSIONS_INCLUDE_DIR)
    add_library(displayxr_ext_headers INTERFACE)
    target_include_directories(displayxr_ext_headers INTERFACE
        "${DISPLAYXR_EXTENSIONS_INCLUDE_DIR}")
else()
    FetchContent_Declare(
        displayxr_extensions_headers
        GIT_REPOSITORY https://github.com/DisplayXR/displayxr-extensions.git
        GIT_TAG fee5d51071422ed5f08164db2bb043e2401e5980
    )
    FetchContent_GetProperties(displayxr_extensions_headers)
    if(NOT displayxr_extensions_headers_POPULATED)
        FetchContent_Populate(displayxr_extensions_headers)
    endif()
    add_library(displayxr_ext_headers INTERFACE)
    target_include_directories(displayxr_ext_headers INTERFACE
        "${displayxr_extensions_headers_SOURCE_DIR}/include")
endif()

# ---------------------------------------------------------------------------
# displayxr::common — C++17 app scaffolding (epic #396 W4, re-scoped #393)
#
# The shared application plumbing that was copy-vendored (and drifting) across
# the runtime's test_apps/common and both standalone demos: logging, Win32
# input + window management, OpenXR session/swapchain/frame lifecycle, D2D /
# DirectWrite text + HUD, the D3D11 reference renderer, window-space-layer UI
# helpers, the thin app-side atlas-capture helper (filename numbering + flash
# overlay; the GPU readback itself is the runtime's xrCaptureAtlasEXT), and
# the vendored stb image headers (this lib owns exactly one read- and one
# write-implementation TU per platform — consumers must not define
# STB_IMAGE_IMPLEMENTATION / STB_IMAGE_WRITE_IMPLEMENTATION themselves).
#
# Single target, platform-gated sources: Windows apps get the full stack;
# Apple consumers get the cross-platform subset + the macOS HUD / capture
# helpers. Linked by C++ APPS only — engines (Unity/Unreal) link just
# displayxr::math.
#
# Divergence policy (epic #396): parameterize/inject at the call site
# (e.g. InputState::hudToggleRequiresShift, EndFrame's projectionLayerFlags),
# never #ifdef APP in here.
#
# NOTE: leia_math.h stays app-local (see the math note above).
# ---------------------------------------------------------------------------
set(DISPLAYXR_COMMON_SOURCES
    common/atlas_capture.h
    common/view_params.h
    common/mip_chain.h
    common/xr_window_space_hud.cpp
    common/xr_window_space_hud.h
)
if(WIN32)
    list(APPEND DISPLAYXR_COMMON_SOURCES
        common/logging.cpp
        common/logging.h
        common/input_handler.cpp
        common/input_handler.h
        common/window_manager.cpp
        common/window_manager.h
        common/xr_session_common.cpp
        common/xr_session_common.h
        common/atlas_capture.cpp
        common/optimus_dgpu_hint.c
        common/d3d11_renderer.cpp
        common/d3d11_renderer.h
        common/text_overlay.cpp
        common/text_overlay.h
        common/hud_renderer.cpp
        common/hud_renderer.h
    )
elseif(APPLE)
    list(APPEND DISPLAYXR_COMMON_SOURCES
        common/atlas_capture_macos.mm
        common/hud_renderer_macos.mm
        common/hud_renderer_macos.h
        common/stb_image_impl_macos.cpp
    )
endif()

add_library(displayxr_common_lib STATIC ${DISPLAYXR_COMMON_SOURCES})
add_library(displayxr::common ALIAS displayxr_common_lib)

target_include_directories(displayxr_common_lib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/common>
    $<INSTALL_INTERFACE:common>
)
# displayxr_ext_headers BEFORE ${DISPLAYXR_MATH_OPENXR}: when the consumer's
# extensions dir also carries the Khronos set, it must win the include order.
target_link_libraries(displayxr_common_lib PUBLIC
    displayxr::math
    displayxr_ext_headers
    ${DISPLAYXR_MATH_OPENXR}
)
set_target_properties(displayxr_common_lib PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

if(WIN32)
    target_link_libraries(displayxr_common_lib PUBLIC
        d3d11 dxgi d3dcompiler d2d1 dwrite)
    # Hybrid-GPU dGPU hint: optimus_dgpu_hint.c defines NvOptimusEnablement /
    # AmdPowerXpressRequestHighPerformance, but a static-library member with no
    # referenced symbols would be discarded — force-include it into every
    # consumer executable via an INTERFACE link option.
    if(MSVC)
        target_link_options(displayxr_common_lib INTERFACE
            "/INCLUDE:NvOptimusEnablement")
    else()
        target_link_options(displayxr_common_lib INTERFACE
            "-Wl,--undefined=NvOptimusEnablement")
    endif()
elseif(APPLE)
    target_link_libraries(displayxr_common_lib PUBLIC
        "-framework AppKit"
        "-framework CoreGraphics"
        "-framework CoreText"
        "-framework Foundation"
        "-framework QuartzCore"
    )
endif()

# Workspace-app manifest helper (displayxr_install_manifest()). Function
# definitions are global once this include runs, so any consumer that
# FetchContent_MakeAvailable's this project can call it directly.
include(common/displayxr_manifest.cmake)

if(DISPLAYXR_COMMON_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

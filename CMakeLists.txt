cmake_minimum_required(VERSION 3.16)

project(displayxr_common
    VERSION 0.1.0
    DESCRIPTION "Shared off-axis Kooima projection math for DisplayXR 3D displays"
    LANGUAGES C)

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

if(DISPLAYXR_COMMON_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

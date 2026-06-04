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
# Reuse the consumer's OpenXR if it already provides one (most consumers — the
# runtime test apps, demos, engine plug-ins — already find_package or vendor
# OpenXR). Re-declaring the OpenXR targets here would collide ("target already
# exists"). Only fetch the headers when nothing is available (e.g. the
# standalone CI selftest build).
if(NOT TARGET OpenXR::headers AND NOT TARGET OpenXR::openxr_loader)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
    set(BUILD_LOADER OFF CACHE BOOL "" FORCE)
    set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        OpenXR
        GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
        GIT_TAG release-1.1.43
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(OpenXR)
endif()

# Link whichever OpenXR target carries the headers (we only need the Xr* types).
if(TARGET OpenXR::headers)
    set(DISPLAYXR_MATH_OPENXR OpenXR::headers)
else()
    set(DISPLAYXR_MATH_OPENXR OpenXR::openxr_loader)
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

# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
#
# displayxr::linux_window — THE desktop-Linux app window for DisplayXR apps
# (dxr_linux_window.{h,cpp}): X11 or native Wayland in one binary, chosen by
# capability at startup, plus the header bar (displayxr::csd), input events,
# transparency / click-through and the phase-snapped drag. Consumed by the
# runtime's Linux test apps and by every demo; see dxr_linux_window.h.
#
# Included from the top-level CMakeLists.txt on desktop Linux only. Usage:
#
#     FetchContent_MakeAvailable(displayxr_common)
#     target_link_libraries(my_app PRIVATE displayxr::linux_window)
#
# X11 (libx11-dev) is REQUIRED for the target to exist; without it the target
# is simply not defined (a consumer can test `if(TARGET displayxr::linux_window)`).
# Everything else is optional and detected here, each one only disabling its
# own nicety:
#   libxrandr-dev     _NET_WM_FULLSCREEN_MONITORS targets the panel's monitor
#   libxext-dev       XShape click-through input regions on X11
#   libwayland-dev    the native-Wayland leg (DXR_APP_HAVE_WAYLAND) — also
#                     needs wayland-scanner (libwayland-bin); the protocol XML
#                     is vendored in wayland-protocols/, so wayland-protocols
#                     need not be installed
#   libxkbcommon-dev  layout-aware keysyms on Wayland (else a US table)
#   libdbus-1-dev     the phase-snapped Wayland drag (the compositor's drag
#                     lattice, dxr_wl_placement). The Wayland-ready probe
#                     loads libdbus-1 at RUN time and needs no build dependency.
#
# The OpenXR headers come from the same place displayxr::common takes them
# (displayxr_ext_headers + the Khronos set), so XR_DXR_xlib_window_binding.h,
# XR_DXR_wayland_surface_binding.h and XR_DXR_weave.h must be in the
# consumer's DISPLAYXR_EXTENSIONS_INCLUDE_DIR (or the fetched extensions).
#
# xrGetInstanceProcAddr is referenced (attach_session, DxrWeaveSnap) but not
# linked here: the app links the OpenXR loader, as it must anyway.

set(_dxr_lw_dir "${CMAKE_CURRENT_LIST_DIR}")

find_package(X11 QUIET)
if(NOT X11_FOUND)
    message(STATUS "displayxr::linux_window: libX11 not found — target not defined")
    return()
endif()

add_library(displayxr_linux_window STATIC
    "${_dxr_lw_dir}/dxr_linux_window.cpp"
    "${_dxr_lw_dir}/dxr_linux_window.h"
    "${_dxr_lw_dir}/dxr_x11_chrome.cpp"
    "${_dxr_lw_dir}/dxr_x11_chrome.h"
    "${_dxr_lw_dir}/dxr_weave_snap.h"
)
add_library(displayxr::linux_window ALIAS displayxr_linux_window)
set_target_properties(displayxr_linux_window PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(displayxr_linux_window PUBLIC
    "${_dxr_lw_dir}"
    # u_x11_scale.h / util/u_wayland_geom.h: header-only placement + scale
    # arithmetic shared VERBATIM with the runtime's src/xrt/auxiliary/util (the
    # runtime's test-app build fails if the two copies ever differ).
    "${_dxr_lw_dir}/xrt_aux/util"
    "${_dxr_lw_dir}/xrt_aux"
)
target_link_libraries(displayxr_linux_window PUBLIC
    displayxr::csd
    displayxr_ext_headers
    ${DISPLAYXR_MATH_OPENXR}
    X11::X11
    ${CMAKE_DL_LIBS}
)

if(TARGET X11::Xrandr)
    target_link_libraries(displayxr_linux_window PRIVATE X11::Xrandr)
    target_compile_definitions(displayxr_linux_window PRIVATE DXR_APP_HAVE_XRANDR)
else()
    message(STATUS "displayxr::linux_window: Xrandr NOT found — X11 fullscreen falls back to the "
                   "window's current output (install libxrandr-dev)")
endif()

if(TARGET X11::Xext AND X11_Xshape_INCLUDE_PATH)
    target_link_libraries(displayxr_linux_window PRIVATE X11::Xext)
    target_compile_definitions(displayxr_linux_window PRIVATE DXR_LW_HAVE_XSHAPE)
else()
    message(STATUS "displayxr::linux_window: libXext/XShape NOT found — no X11 click-through "
                   "(install libxext-dev)")
endif()

find_package(PkgConfig QUIET)
set(_dxr_lw_wl FALSE)
set(_dxr_lw_scanner "")
if(PkgConfig_FOUND)
    pkg_check_modules(DXR_LW_WAYLAND_CLIENT QUIET wayland-client)
    if(DXR_LW_WAYLAND_CLIENT_FOUND)
        pkg_check_modules(DXR_LW_WAYLAND_SCANNER_PC QUIET wayland-scanner)
        if(DXR_LW_WAYLAND_SCANNER_PC_FOUND)
            pkg_get_variable(_dxr_lw_scanner wayland-scanner wayland_scanner)
        endif()
        if(NOT _dxr_lw_scanner)
            find_program(_dxr_lw_scanner NAMES wayland-scanner)
        endif()
        if(_dxr_lw_scanner)
            set(_dxr_lw_wl TRUE)
        endif()
    endif()
    pkg_check_modules(DXR_LW_XKBCOMMON QUIET xkbcommon)
endif()

if(NOT _dxr_lw_wl)
    message(STATUS "displayxr::linux_window: libwayland-client / wayland-scanner NOT found — X11-only build "
                   "(XR_DXR_wayland_surface_binding backend compiled out)")
    return()
endif()

# One entry per protocol, named by its XML STEM — both the file's basename and
# the prefix wayland-scanner gives its outputs, and so the name the #includes
# use. xdg-decoration / cursor-shape (+ tablet-v2, which cursor-shape
# references) / ext-background-effect serve the title bar (dxr_wl_chrome.cpp).
set(_dxr_lw_gen "${CMAKE_CURRENT_BINARY_DIR}/displayxr-linux-window-wayland")
set(_dxr_lw_generated "")
foreach(_stem "xdg-shell" "xdg-output-unstable-v1" "viewporter" "fractional-scale-v1"
              "xdg-decoration-unstable-v1" "cursor-shape-v1" "tablet-v2"
              "ext-background-effect-v1")
    set(_xml "${_dxr_lw_dir}/wayland-protocols/${_stem}.xml")
    set(_hdr "${_dxr_lw_gen}/${_stem}-client-protocol.h")
    set(_src "${_dxr_lw_gen}/${_stem}-protocol.c")
    add_custom_command(
        OUTPUT "${_hdr}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_dxr_lw_gen}"
        COMMAND "${_dxr_lw_scanner}" client-header "${_xml}" "${_hdr}"
        DEPENDS "${_xml}"
        COMMENT "wayland-scanner client-header ${_stem}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${_src}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_dxr_lw_gen}"
        COMMAND "${_dxr_lw_scanner}" private-code "${_xml}" "${_src}"
        DEPENDS "${_xml}"
        COMMENT "wayland-scanner private-code ${_stem}"
        VERBATIM
    )
    list(APPEND _dxr_lw_generated "${_hdr}" "${_src}")
endforeach()

target_sources(displayxr_linux_window PRIVATE
    ${_dxr_lw_generated}
    "${_dxr_lw_dir}/dxr_wl_chrome.cpp"
    "${_dxr_lw_dir}/dxr_wl_chrome.h"
    "${_dxr_lw_dir}/dxr_wl_placement.cpp"
    "${_dxr_lw_dir}/dxr_wl_placement.h"
)

# libdbus-1 (optional, libdbus-1-dev): the client of the compositor's drag
# lattice (dxr_wl_placement, runtime #1609/#1686), which keeps the interlace
# phase still while the compositor drags a native-Wayland window. Without it
# the Wayland drag runs unconstrained, exactly as before.
if(PkgConfig_FOUND)
    pkg_check_modules(DXR_LW_DBUS QUIET dbus-1)
endif()
if(DXR_LW_DBUS_FOUND)
    target_include_directories(displayxr_linux_window PRIVATE ${DXR_LW_DBUS_INCLUDE_DIRS})
    target_link_directories(displayxr_linux_window PUBLIC ${DXR_LW_DBUS_LIBRARY_DIRS})
    target_link_libraries(displayxr_linux_window PRIVATE ${DXR_LW_DBUS_LIBRARIES})
    target_compile_definitions(displayxr_linux_window PRIVATE DXR_APP_HAVE_DBUS)
    set(_dxr_lw_lattice "drag lattice ENABLED (libdbus ${DXR_LW_DBUS_VERSION})")
else()
    set(_dxr_lw_lattice "libdbus-1 NOT found — Wayland drags unconstrained (install libdbus-1-dev)")
endif()
target_include_directories(displayxr_linux_window PRIVATE "${_dxr_lw_gen}")
# PUBLIC: the header's class layout depends on these (the Wayland members), so
# every TU that includes it must see the same definitions as the library.
target_include_directories(displayxr_linux_window PUBLIC ${DXR_LW_WAYLAND_CLIENT_INCLUDE_DIRS})
target_link_directories(displayxr_linux_window PUBLIC ${DXR_LW_WAYLAND_CLIENT_LIBRARY_DIRS})
target_link_libraries(displayxr_linux_window PUBLIC ${DXR_LW_WAYLAND_CLIENT_LIBRARIES})
target_compile_definitions(displayxr_linux_window PUBLIC DXR_APP_HAVE_WAYLAND DXR_APP_HAVE_WL_CHROME)

if(DXR_LW_XKBCOMMON_FOUND)
    target_include_directories(displayxr_linux_window PRIVATE ${DXR_LW_XKBCOMMON_INCLUDE_DIRS})
    target_link_directories(displayxr_linux_window PRIVATE ${DXR_LW_XKBCOMMON_LIBRARY_DIRS})
    target_link_libraries(displayxr_linux_window PRIVATE ${DXR_LW_XKBCOMMON_LIBRARIES})
    target_compile_definitions(displayxr_linux_window PRIVATE DXR_LW_HAVE_XKBCOMMON)
    set(_dxr_lw_xkb "xkbcommon ${DXR_LW_XKBCOMMON_VERSION}")
else()
    set(_dxr_lw_xkb "no xkbcommon — US-layout keysym table")
endif()

message(STATUS "displayxr::linux_window: Wayland backend ENABLED "
               "(wayland-client ${DXR_LW_WAYLAND_CLIENT_VERSION}, ${_dxr_lw_xkb}, ${_dxr_lw_lattice})")

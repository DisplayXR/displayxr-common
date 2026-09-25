add_executable(displayxr_math_selftest selftest.c)
target_link_libraries(displayxr_math_selftest PRIVATE displayxr::math)
set_target_properties(displayxr_math_selftest PROPERTIES C_STANDARD 11)

add_test(NAME displayxr_math_selftest COMMAND displayxr_math_selftest)

# displayxr::common smoke test (#396 W4): pure-helper checks + header-chain
# compile + link-closure proof. Runs GPU-free on the bare CI runners.
add_executable(displayxr_common_smoke common_smoke.cpp)
target_link_libraries(displayxr_common_smoke PRIVATE displayxr::common)
set_target_properties(displayxr_common_smoke PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

add_test(NAME displayxr_common_smoke COMMAND displayxr_common_smoke)

# Launch-contract tests (launch_args.h parser + policy, url_fetch.h pure
# helpers). Platform-neutral; the security negatives are the contract.
add_executable(displayxr_launch_args_test launch_args_test.cpp)
target_link_libraries(displayxr_launch_args_test PRIVATE displayxr::common)
set_target_properties(displayxr_launch_args_test PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

add_test(NAME displayxr_launch_args_test COMMAND displayxr_launch_args_test)

# The desktop-Linux fetcher (url_fetch.cpp, libcurl via dlopen). Live checks
# only with DXR_URL_FETCH_TEST_PORT set (CI starts url_fetch_server.py).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(displayxr_url_fetch_test url_fetch_test.cpp)
    target_link_libraries(displayxr_url_fetch_test PRIVATE displayxr::common)
    set_target_properties(displayxr_url_fetch_test PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    add_test(NAME displayxr_url_fetch_test COMMAND displayxr_url_fetch_test)
endif()

# dxr_view_config.h contract test (runtime #1486). Links displayxr::rules (the
# header-only, platform-neutral target the non-Windows legs consume) plus the
# header-only OpenXR / DisplayXR-extension include targets — NOT the loader:
# the C TU defines its own xrEnumerateViewConfigurations, which is what makes
# the probe loop testable on a bare CI runner. Two TUs on purpose: C11 + C++17.
add_executable(displayxr_view_config_test
    view_config_test.c
    view_config_test_cxx.cpp)
target_link_libraries(displayxr_view_config_test PRIVATE
    displayxr::rules
    displayxr_ext_headers
    ${DISPLAYXR_MATH_OPENXR})
set_target_properties(displayxr_view_config_test PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

add_test(NAME displayxr_view_config_test COMMAND displayxr_view_config_test)

# csd_titlebar contract test (displayxr-common#52): rounded-corner alpha, hit
# testing, packing, double-click. Window-system free, so it runs everywhere.
add_executable(displayxr_csd_test csd_test.cpp)
target_link_libraries(displayxr_csd_test PRIVATE displayxr::csd)
set_target_properties(displayxr_csd_test PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

add_test(NAME displayxr_csd_test COMMAND displayxr_csd_test)

# displayxr::linux_window contract test (desktop Linux only): the --platform
# parser and the capability-probe selection everywhere; with
# DXR_LW_TEST_X11=1 (CI: under xvfb-run) real X11 windows through create ->
# events -> fullscreen -> destroy.
if(TARGET displayxr::linux_window)
    add_executable(displayxr_linux_window_test linux_window_test.cpp)
    target_link_libraries(displayxr_linux_window_test PRIVATE displayxr::linux_window)
    set_target_properties(displayxr_linux_window_test PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    add_test(NAME displayxr_linux_window_test COMMAND displayxr_linux_window_test)
endif()

# The Wayland drag lattice's table build at any output scale (pure; runs on
# every Linux leg, no display needed).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(displayxr_linux_window_lattice_test linux_window_lattice_test.cpp)
    target_include_directories(displayxr_linux_window_lattice_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../common/linux")
    set_target_properties(displayxr_linux_window_lattice_test PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    add_test(NAME displayxr_linux_window_lattice_test COMMAND displayxr_linux_window_lattice_test)
endif()

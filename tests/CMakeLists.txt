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

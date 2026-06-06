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

# C++ unit tests for jeff (GoogleTest). Run with `make test` or `ctest` in the build tree.
#
# One test binary per area under test; add a source here and it is discovered by ctest.

include(${PROJECT_SOURCE_DIR}/cmake/FetchGoogleTest.cmake)

add_executable(jeff_json_test json_test.cpp)
target_link_libraries(jeff_json_test PRIVATE jeff_json GTest::gtest_main jeff_compiler_options)
gtest_discover_tests(jeff_json_test)

add_executable(router_common_test router_common_test.cpp)
target_include_directories(router_common_test PRIVATE ${PROJECT_SOURCE_DIR}/src/cli)
target_link_libraries(router_common_test PRIVATE jeff_json GTest::gtest_main jeff_compiler_options)
gtest_discover_tests(router_common_test)

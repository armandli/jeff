# GoogleTest for the C++ unit tests. Shared by the top-level jeff build (tests/) and the
# standalone jfsh build (jfsh/tests/) so both pin the same release.
#
# Provides GTest::gtest, GTest::gtest_main, GTest::gmock and the gtest_discover_tests()
# command from CMake's GoogleTest module.

include_guard(GLOBAL)
include(FetchContent)

if(NOT TARGET GTest::gtest_main)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.17.0
    GIT_SHALLOW    TRUE
  )
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  # System headers: keep strict consumer warnings (-Wconversion, -Wshadow) off gtest code.
  foreach(_gtest_target gtest gtest_main gmock gmock_main)
    if(TARGET ${_gtest_target})
      get_target_property(_gtest_inc ${_gtest_target} INTERFACE_INCLUDE_DIRECTORIES)
      set_target_properties(${_gtest_target} PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_gtest_inc}")
    endif()
  endforeach()
endif()

include(GoogleTest)

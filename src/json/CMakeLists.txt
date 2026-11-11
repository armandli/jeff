# jeff_json — the repo's JSON layer (simdjson + gap-filling utilities).
#
# Shared by the top-level jeff build and the standalone jfsh build; jfsh pulls it in with
# add_subdirectory(../src/json). This file owns the simdjson dependency so both builds pin
# the same release.

include(FetchContent)

if(NOT TARGET simdjson)
  FetchContent_Declare(simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson.git
    GIT_TAG        tags/v5.0.2
    GIT_SHALLOW    TRUE
  )
  set(SIMDJSON_DEVELOPER_MODE OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(simdjson)

  # Treat simdjson headers as system headers so strict consumer warnings
  # (-Wconversion, -Wsign-conversion) are not reported against its code.
  get_target_property(_simdjson_inc simdjson INTERFACE_INCLUDE_DIRECTORIES)
  set_target_properties(simdjson PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_simdjson_inc}")
endif()

add_library(jeff_json STATIC json.cpp)
add_library(jeff::json ALIAS jeff_json)

target_include_directories(jeff_json PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(jeff_json PUBLIC cxx_std_20)
target_compile_options(jeff_json PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(jeff_json PROPERTIES POSITION_INDEPENDENT_CODE ON)

# simdjson stays PRIVATE: json.hpp does not include simdjson.h.
target_link_libraries(jeff_json PRIVATE simdjson)

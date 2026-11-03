add_library(jeff_mcp STATIC
  server.cpp
)

target_include_directories(jeff_mcp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(jeff_mcp PUBLIC jeff_inference jeff_compiler_options)

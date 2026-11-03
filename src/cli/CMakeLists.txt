add_executable(jeff main.cpp)
target_link_libraries(jeff PRIVATE jeff_inference jeff_mcp jeff_compiler_options)

add_executable(jeff-serve serve.cpp)
target_link_libraries(jeff-serve PRIVATE jeff_inference jeff_mcp jeff_compiler_options)

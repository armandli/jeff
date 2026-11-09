add_executable(jeff main.cpp)
target_link_libraries(jeff PRIVATE jeff_inference jeff_mcp jeff_compiler_options)

add_executable(jeff-serve serve.cpp)
target_link_libraries(jeff-serve PRIVATE jeff_inference jeff_mcp jeff_compiler_options)

# Router CLIs — no libtorch dependency; inference is delegated to Python via popen.
add_executable(bash_router bash_router.cpp)
target_link_libraries(bash_router PRIVATE jeff_compiler_options)

add_executable(zsh_router zsh_router.cpp)
target_link_libraries(zsh_router PRIVATE jeff_compiler_options)

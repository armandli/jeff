add_executable(jeff main.cpp)
target_link_libraries(jeff PRIVATE jeff_inference jeff_mcp jeff_compiler_options)

add_executable(jeff-serve serve.cpp)
target_link_libraries(jeff-serve PRIVATE jeff_inference jeff_mcp jeff_compiler_options)

# Router CLIs — no libtorch dependency; inference is delegated to Python via popen.
# JEFF_SOURCE_DIR is the default repo root, so the binaries work from the build dir.
foreach(router bash_router zsh_router)
    add_executable(${router} ${router}.cpp)
    target_link_libraries(${router} PRIVATE jeff_compiler_options)
    target_compile_definitions(${router} PRIVATE JEFF_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
endforeach()

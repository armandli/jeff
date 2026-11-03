cmake_minimum_required(VERSION 3.20)
project(jeff CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# Set TORCH_DIR to your LibTorch install, e.g.:
#   cmake -DTORCH_DIR=/path/to/libtorch/share/cmake/Torch ..
# Or set the environment variable before running cmake.
if(DEFINED ENV{TORCH_DIR})
  set(TORCH_DIR $ENV{TORCH_DIR})
endif()

find_package(Torch REQUIRED HINTS "${TORCH_DIR}")

include(cmake/CompilerOptions.cmake)

add_subdirectory(src/inference)
add_subdirectory(src/mcp)
add_subdirectory(src/cli)

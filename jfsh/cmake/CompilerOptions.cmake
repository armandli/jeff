# jfsh compiler options — C++26 with GCC 15 / Clang 21
add_library(jfsh_compiler_options INTERFACE)

target_compile_features(jfsh_compiler_options INTERFACE cxx_std_26)

target_compile_options(jfsh_compiler_options INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Wno-unused-parameter          # large stub phase; remove once implemented
  $<$<CXX_COMPILER_ID:GNU>:
      -Wduplicated-cond
      -Wlogical-op
      -fdiagnostics-color=always
  >
  $<$<CXX_COMPILER_ID:Clang>:
      -Wno-gnu-zero-variadic-macro-arguments
      -fcolor-diagnostics
  >
  $<$<CONFIG:Release>: -O3 -march=native -DNDEBUG>
  $<$<CONFIG:Debug>:   -O0 -g -fno-omit-frame-pointer>
  $<$<CONFIG:RelWithDebInfo>: -O2 -g>
)

target_link_options(jfsh_compiler_options INTERFACE
  $<$<CONFIG:Debug>:>
)

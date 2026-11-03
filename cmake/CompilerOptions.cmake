add_library(jeff_compiler_options INTERFACE)

target_compile_options(jeff_compiler_options INTERFACE
  -Wall -Wextra -Wpedantic
  $<$<CONFIG:Release>:-O3 -march=native>
  $<$<CONFIG:Debug>:-g -O0 -fsanitize=address>
)

target_link_options(jeff_compiler_options INTERFACE
  $<$<CONFIG:Debug>:-fsanitize=address>
)

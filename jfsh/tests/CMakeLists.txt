add_executable(jfsh_tests placeholder.cpp)
target_link_libraries(jfsh_tests PRIVATE
  jfsh_core jfsh_lexer jfsh_parser jfsh_exec
  jfsh_glob jfsh_builtins jfsh_router jfsh_agent
  jfsh_compiler_options
)
add_test(NAME jfsh_placeholder COMMAND jfsh_tests)

# jfsh unit tests (GoogleTest). GoogleTest is pinned once for the whole repo in
# ../cmake/FetchGoogleTest.cmake; the jeff_json tests live in the top-level tests/.

include(${PROJECT_SOURCE_DIR}/../cmake/FetchGoogleTest.cmake)

add_executable(jfsh_tests
  agent_test.cpp
  compound_test.cpp
  cond_test.cpp
  exec_test.cpp
  glob_test.cpp
  history_test.cpp
  interactive_test.cpp
  jobs_test.cpp
  lexer_test.cpp
  math_test.cpp
  params_test.cpp
  parser_test.cpp
  signals_test.cpp
  subst_test.cpp
)
target_link_libraries(jfsh_tests PRIVATE
  jfsh_core jfsh_lexer jfsh_parser jfsh_exec jfsh_glob jfsh_builtins jfsh_history jfsh_agent
  GTest::gtest_main
  jfsh_compiler_options
)
# The interactive tests drive the jfsh binary through a pseudo-terminal.
add_dependencies(jfsh_tests jfsh)
target_compile_definitions(jfsh_tests PRIVATE JFSH_BINARY="$<TARGET_FILE:jfsh>")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(jfsh_tests PRIVATE util)   # forkpty
endif()

gtest_discover_tests(jfsh_tests)

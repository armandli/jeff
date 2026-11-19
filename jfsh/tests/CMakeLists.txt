# jfsh unit tests (GoogleTest). GoogleTest is pinned once for the whole repo in
# ../cmake/FetchGoogleTest.cmake; the jeff_json tests live in the top-level tests/.

include(${PROJECT_SOURCE_DIR}/../cmake/FetchGoogleTest.cmake)

add_executable(jfsh_tests
  agent_test.cpp
  builtins_test.cpp
  compound_test.cpp
  cond_test.cpp
  editor_test.cpp
  exec_test.cpp
  glob_test.cpp
  history_test.cpp
  interactive_test.cpp
  jobs_test.cpp
  lexer_test.cpp
  math_test.cpp
  modules_test.cpp
  modules_m11_test.cpp
  params_test.cpp
  parser_test.cpp
  prompt_test.cpp
  signals_test.cpp
  startup_test.cpp
  subst_test.cpp
)
target_link_libraries(jfsh_tests PRIVATE
  # jfsh_exec brings jfsh_parser, jfsh_lexer, jfsh_core, jfsh_glob, jfsh_builtins,
  # jfsh_history, jfsh_prompt and jfsh_editor; naming them here too put them on the
  # link line twice, which Apple's ld warns about.
  jfsh_exec jfsh_agent
  GTest::gtest_main
  jfsh_compiler_options
)
# The interactive tests drive the jfsh binary through a pseudo-terminal.
add_dependencies(jfsh_tests jfsh)
target_compile_definitions(jfsh_tests PRIVATE JFSH_BINARY="$<TARGET_FILE:jfsh>")
# Lets the tests include the shared fixture as <shell_fixture.h>.
target_include_directories(jfsh_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(jfsh_tests PRIVATE util)   # forkpty
endif()

gtest_discover_tests(jfsh_tests)

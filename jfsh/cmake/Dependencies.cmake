# jfsh external dependencies
include(FetchContent)

# ---- ncurses (terminal control for the line editor) -------------------------
# Optional: the editor falls back to raw-termios mode when curses is absent.
find_package(Curses QUIET)
add_library(jfsh_dep_curses INTERFACE IMPORTED)
if(CURSES_FOUND)
  target_include_directories(jfsh_dep_curses INTERFACE ${CURSES_INCLUDE_DIRS})
  target_link_libraries(jfsh_dep_curses INTERFACE ${CURSES_LIBRARIES})
  target_compile_definitions(jfsh_dep_curses INTERFACE JFSH_HAVE_CURSES)
  message(STATUS "jfsh: ncurses found — advanced terminal support enabled")
else()
  message(STATUS "jfsh: ncurses not found — using raw termios (install libncurses-dev for full ZLE)")
endif()

# ---- libcurl (HTTP for LLM provider APIs) ------------------------------------
find_package(CURL REQUIRED)
add_library(jfsh_dep_curl INTERFACE IMPORTED)
target_include_directories(jfsh_dep_curl INTERFACE ${CURL_INCLUDE_DIRS})
target_link_libraries(jfsh_dep_curl INTERFACE ${CURL_LIBRARIES})

# ---- OpenSSL (TLS for HTTPS API calls) ---------------------------------------
find_package(OpenSSL REQUIRED)
add_library(jfsh_dep_ssl INTERFACE IMPORTED)
target_link_libraries(jfsh_dep_ssl INTERFACE OpenSSL::SSL OpenSSL::Crypto)

# ---- jeff_json (simdjson + utilities, shared with the top-level jeff build) ---
# Lives in the parent repo at src/json; it fetches and pins simdjson itself.
if(NOT TARGET jeff_json)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../src/json
                   ${CMAKE_CURRENT_BINARY_DIR}/jeff_json)
endif()

# (LibTorch is not used by the shell — inference runs in jeff-server.)

# ---- PCRE2 (zsh/pcre module in M10 Phase 2c) --------------------------------
# Optional: when PCRE2 is missing, the pcre_* builtins return "not built in"
# but zsh/pcre still registers so `zmodload zsh/pcre` prints the expected
# error wording.  Debian/Ubuntu: `apt-get install libpcre2-dev`.
find_package(PkgConfig QUIET)
add_library(jfsh_dep_pcre2 INTERFACE IMPORTED)
if(PkgConfig_FOUND)
  pkg_check_modules(PCRE2 QUIET libpcre2-8)
endif()
if(PCRE2_FOUND)
  target_include_directories(jfsh_dep_pcre2 INTERFACE ${PCRE2_INCLUDE_DIRS})
  target_link_libraries(jfsh_dep_pcre2 INTERFACE ${PCRE2_LIBRARIES})
  target_compile_definitions(jfsh_dep_pcre2 INTERFACE JFSH_HAVE_PCRE2
                             PCRE2_CODE_UNIT_WIDTH=8)
  message(STATUS "jfsh: PCRE2 found — zsh/pcre fully functional")
else()
  message(STATUS "jfsh: PCRE2 not found — zsh/pcre builtins will be stubs (install libpcre2-dev)")
endif()

# ---- threads (async agent calls) --------------------------------------------
find_package(Threads REQUIRED)

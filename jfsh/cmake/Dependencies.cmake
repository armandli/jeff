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

# ---- threads (async agent calls) --------------------------------------------
find_package(Threads REQUIRED)

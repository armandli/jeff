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

# ---- nlohmann/json (JSON parsing for LLM API requests/responses) ------------
FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# ---- LibTorch (optional — System 1 router TorchScript mode) -----------------
# Set JFSH_TORCH_DIR or env TORCH_DIR to enable in-process model loading.
# Without it the router falls back to Python subprocess mode.
option(JFSH_USE_TORCH "Enable in-process TorchScript router (requires LibTorch)" OFF)
if(JFSH_USE_TORCH)
  if(DEFINED ENV{TORCH_DIR})
    set(TORCH_DIR $ENV{TORCH_DIR})
  endif()
  find_package(Torch REQUIRED HINTS "${TORCH_DIR}")
  add_library(jfsh_dep_torch INTERFACE IMPORTED)
  target_include_directories(jfsh_dep_torch INTERFACE ${TORCH_INCLUDE_DIRS})
  target_link_libraries(jfsh_dep_torch INTERFACE ${TORCH_LIBRARIES})
  target_compile_definitions(jfsh_dep_torch INTERFACE JFSH_HAVE_TORCH ${TORCH_CXX_FLAGS})
  message(STATUS "jfsh: TorchScript router enabled")
else()
  add_library(jfsh_dep_torch INTERFACE)   # empty placeholder
  message(STATUS "jfsh: TorchScript router disabled (subprocess mode only)")
endif()

# ---- cpp-httplib (single-header HTTP server for jeff-service) ---------------
FetchContent_Declare(cpp_httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG        v0.18.5
  GIT_SHALLOW    TRUE
)
# Header-only mode — avoids a separate compilation unit in consumer targets.
set(HTTPLIB_HEADER_ONLY ON  CACHE BOOL "" FORCE)
set(HTTPLIB_COMPILE     OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cpp_httplib)

# ---- threads (async agent calls) --------------------------------------------
find_package(Threads REQUIRED)

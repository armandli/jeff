cmake_minimum_required(VERSION 3.28)
project(jfsh
  VERSION     0.1.0
  DESCRIPTION "Jeff Shell — a zsh-compatible shell with integrated System 1 LLM routing"
  LANGUAGES   CXX
)

# ---------------------------------------------------------------------------
# C++26 — required throughout. GCC 15 and Clang 21 both support it.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD          26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerOptions)
include(Dependencies)

# ---------------------------------------------------------------------------
# jfsh_core — types, memory, hash tables, linked lists, string ops, params,
#             options, jobs, signals. Mirrors zsh Src/{hashtable,linklist,
#             mem,string,utils,sort,params,options,jobs,signals,compat}.c
# ---------------------------------------------------------------------------
add_library(jfsh_core STATIC
  src/core/init.cpp
  src/core/params.cpp
  src/core/options.cpp
  src/core/jobs.cpp
  src/core/signals.cpp
  src/core/hashtable.cpp
  src/core/string.cpp
  src/core/mem.cpp
  src/core/utils.cpp
  src/core/sort.cpp
)
target_include_directories(jfsh_core PUBLIC include)
target_link_libraries(jfsh_core PUBLIC jfsh_compiler_options Threads::Threads)

# ---------------------------------------------------------------------------
# jfsh_lexer — tokeniser and input management.
#              Mirrors zsh Src/{lex,input}.c
# ---------------------------------------------------------------------------
add_library(jfsh_lexer STATIC
  src/lexer/lexer.cpp
  src/lexer/input.cpp
)
target_include_directories(jfsh_lexer PUBLIC include)
target_link_libraries(jfsh_lexer PUBLIC jfsh_core jfsh_compiler_options)

# ---------------------------------------------------------------------------
# jfsh_parser — recursive-descent parser producing WordCode (Eprog).
#               Mirrors zsh Src/{parse,text}.c
# ---------------------------------------------------------------------------
add_library(jfsh_parser STATIC
  src/parser/parser.cpp
  src/parser/text.cpp
)
target_include_directories(jfsh_parser PUBLIC include)
target_link_libraries(jfsh_parser PUBLIC jfsh_lexer jfsh_compiler_options)

# ---------------------------------------------------------------------------
# jfsh_glob — glob expansion and pattern matching.
#             Mirrors zsh Src/{glob,pattern,hashnameddir}.c
# ---------------------------------------------------------------------------
add_library(jfsh_glob STATIC
  src/glob/glob.cpp
  src/glob/pattern.cpp
)
target_include_directories(jfsh_glob PUBLIC include)
target_link_libraries(jfsh_glob PUBLIC jfsh_core jfsh_compiler_options)

# ---------------------------------------------------------------------------
# jfsh_builtins — built-in commands, conditionals, arithmetic.
#                 Mirrors zsh Src/{builtin,cond,math,loop}.c
# ---------------------------------------------------------------------------
add_library(jfsh_builtins STATIC
  src/builtins/builtins.cpp
  src/builtins/cond.cpp
  src/builtins/math.cpp
)
target_include_directories(jfsh_builtins PUBLIC include)
target_link_libraries(jfsh_builtins PUBLIC jfsh_core jfsh_compiler_options)

# ---------------------------------------------------------------------------
# jfsh_exec — command executor: list → pipeline → command dispatch, context,
#             redirections, job/fork management.
#             word expansion. Mirrors zsh Src/{exec,loop,context,subst}.c
# ---------------------------------------------------------------------------
add_library(jfsh_exec STATIC
  src/exec/executor.cpp
  src/exec/subst.cpp
  src/exec/loop.cpp
  src/exec/context.cpp
)
target_include_directories(jfsh_exec PUBLIC include)
target_link_libraries(jfsh_exec PUBLIC
  jfsh_parser jfsh_glob jfsh_builtins jfsh_compiler_options)

# ---------------------------------------------------------------------------
# jfsh_history — history file, search, deduplication.
#                Mirrors zsh Src/hist.c
# ---------------------------------------------------------------------------
add_library(jfsh_history STATIC
  src/history/history.cpp
)
target_include_directories(jfsh_history PUBLIC include)
target_link_libraries(jfsh_history PUBLIC jfsh_core jfsh_compiler_options)

# ---------------------------------------------------------------------------
# jfsh_prompt — prompt expansion (%n %m %~ %F{} PS1/RPROMPT/PS2).
#               Mirrors zsh Src/prompt.c
# ---------------------------------------------------------------------------
add_library(jfsh_prompt STATIC
  src/prompt/prompt.cpp
)
target_include_directories(jfsh_prompt PUBLIC include)
target_link_libraries(jfsh_prompt PUBLIC jfsh_core jfsh_compiler_options)

# ---------------------------------------------------------------------------
# jfsh_editor — interactive line editor (ZLE equivalent): vi/emacs keymaps,
#               cursor movement, word ops, tab completion, display refresh.
#               Mirrors zsh Src/Zle/{zle_main,zle_bindings,zle_keymap,
#                 zle_vi,zle_move,zle_word,zle_refresh,zle_thingy,zle_hist,
#                 compcore,complete,complist,compmatch,computil}.c
# ---------------------------------------------------------------------------
add_library(jfsh_editor STATIC
  src/editor/editor.cpp
  src/editor/keymap.cpp
  src/editor/vi.cpp
  src/editor/completion.cpp
  src/editor/refresh.cpp
)
target_include_directories(jfsh_editor PUBLIC include)
target_link_libraries(jfsh_editor PUBLIC
  jfsh_core jfsh_history jfsh_prompt jfsh_glob
  jfsh_dep_curses jfsh_compiler_options)

# ---------------------------------------------------------------------------
# jfsh_modules — loadable module system (zmodload equivalent).
#                Mirrors zsh Src/{module,modentry}.c
# ---------------------------------------------------------------------------
add_library(jfsh_modules STATIC
  src/modules/module.cpp
)
target_include_directories(jfsh_modules PUBLIC include)
target_link_libraries(jfsh_modules PUBLIC jfsh_core ${CMAKE_DL_LIBS} jfsh_compiler_options)

# ---------------------------------------------------------------------------
# jfsh_router — System 1 RUN/AGENT classifier.
#               Two backends: subprocess (calls infer.py) and TorchScript
#               (in-process, enabled with -DJFSH_USE_TORCH=ON).
# ---------------------------------------------------------------------------
add_library(jfsh_router STATIC
  src/router/router.cpp
  src/router/jeff.cpp
  src/router/jev.cpp
)
target_include_directories(jfsh_router PUBLIC include)
target_link_libraries(jfsh_router PUBLIC
  jfsh_core jfsh_dep_curl jeff_json jfsh_compiler_options)


# ---------------------------------------------------------------------------
# jfsh_agent — LLM provider abstraction + per-provider HTTP clients.
#              Providers: OpenAI, Anthropic, Ollama, OpenRouter.
# ---------------------------------------------------------------------------
add_library(jfsh_agent STATIC
  src/agent/agent.cpp
  src/agent/provider.cpp
  src/agent/openai.cpp
  src/agent/anthropic.cpp
  src/agent/ollama.cpp
  src/agent/openrouter.cpp
)
target_include_directories(jfsh_agent PUBLIC include)
target_link_libraries(jfsh_agent PUBLIC
  jfsh_core jfsh_dep_curl jfsh_dep_ssl jeff_json
  jfsh_compiler_options)

# ---------------------------------------------------------------------------
# jfsh — the shell binary
# ---------------------------------------------------------------------------
add_executable(jfsh src/main.cpp)
target_link_libraries(jfsh PRIVATE
  jfsh_exec
  jfsh_editor
  jfsh_modules
  jfsh_router
  jfsh_agent
  jfsh_compiler_options
)

install(TARGETS jfsh DESTINATION bin)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
option(JFSH_BUILD_TESTS "Build jfsh unit tests" ON)
if(JFSH_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "  jfsh ${PROJECT_VERSION}")
message(STATUS "  C++ standard  : ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build type    : ${CMAKE_BUILD_TYPE}")
message(STATUS "  jeff router   : connects to jeff-server (127.0.0.1:8765)")
message(STATUS "")

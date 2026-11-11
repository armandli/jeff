# Convenience wrapper around CMake for the C++ part of jeff (src/).
#
#   make                 build Release into build/release
#   make debug           build Debug (with AddressSanitizer) into build/debug
#   make routers         build only bash_router and zsh_router
#   make test            build and run the C++ unit tests (tests/, GoogleTest)
#   make bash_router ARGS="ls -la"   build, then run a binary with ARGS
#   make help            list every target and variable
#
# Variables (override on the command line, e.g. `make BUILD_TYPE=Debug JOBS=4`):
#   BUILD_TYPE   Release | Debug | RelWithDebInfo   (default: Release)
#   BUILD_DIR    build tree                         (default: build/<build_type>)
#   TORCH_DIR    LibTorch cmake dir; default: $TORCH_DIR, else ~/.local/libtorch,
#                else the torch package in python/.venv
#   CUDACXX      nvcc for CUDA LibTorch builds; default: $CUDACXX, else nvcc on PATH,
#                else /usr/local/cuda/bin/nvcc
#   GENERATOR    CMake generator                    (default: Ninja if installed)
#   JOBS         parallel build jobs                (default: nproc)
#   CMAKE_ARGS   extra arguments for the configure step
#
# Configure runs only when BUILD_DIR has no cache or a CMake file changed. After changing
# TORCH_DIR, CUDACXX, GENERATOR or CMAKE_ARGS for an existing BUILD_DIR, run `make reconfigure`.

BUILD_TYPE ?= Release
BUILD_DIR  ?= build/$(shell echo $(BUILD_TYPE) | tr A-Z a-z)
JOBS       ?= $(shell nproc 2>/dev/null || echo 4)
CMAKE      ?= cmake
CMAKE_ARGS ?=

ifeq ($(origin TORCH_DIR),undefined)
  TORCH_DIR := $(firstword $(wildcard \
      $(HOME)/.local/libtorch/share/cmake/Torch \
      $(CURDIR)/python/.venv/lib/python3*/site-packages/torch/share/cmake/Torch))
endif

# A CUDA LibTorch runs enable_language(CUDA), which fails when nvcc is not on PATH.
ifeq ($(origin CUDACXX),undefined)
  CUDACXX := $(or $(shell command -v nvcc 2>/dev/null),$(wildcard /usr/local/cuda/bin/nvcc))
endif

ifeq ($(origin GENERATOR),undefined)
  GENERATOR := $(if $(shell command -v ninja 2>/dev/null),Ninja,Unix Makefiles)
endif

BINARIES := jeff jeff-serve bash_router zsh_router
CACHE    := $(BUILD_DIR)/CMakeCache.txt
CMAKE_SOURCES := CMakeLists.txt $(wildcard cmake/*.cmake) $(wildcard src/*/CMakeLists.txt) \
                 $(wildcard tests/CMakeLists.txt)

.DEFAULT_GOAL := all
.PHONY: all build release debug configure reconfigure routers test $(BINARIES) \
        compile-commands clean distclean rebuild info help

all: build

# Configure only when the build tree is missing or a CMake file changed.
$(CACHE): $(CMAKE_SOURCES)
	@if [ -z "$(TORCH_DIR)" ]; then \
	  echo "error: LibTorch not found; pass TORCH_DIR=/path/to/libtorch/share/cmake/Torch" >&2; \
	  exit 1; fi
	@# A failed configure still writes CMakeCache.txt; delete it so the next make retries.
	$(CMAKE) -S . -B $(BUILD_DIR) -G "$(GENERATOR)" \
	  -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DTORCH_DIR="$(TORCH_DIR)" \
	  $(if $(CUDACXX),-DCMAKE_CUDA_COMPILER="$(CUDACXX)") $(CMAKE_ARGS) \
	  || { rm -f $@; exit 1; }
	@touch $@

configure: $(CACHE)

reconfigure:
	rm -f $(CACHE)
	$(MAKE) --no-print-directory configure

build: $(CACHE)
	$(CMAKE) --build $(BUILD_DIR) -j $(JOBS)

release:
	$(MAKE) --no-print-directory build BUILD_TYPE=Release

debug:
	$(MAKE) --no-print-directory build BUILD_TYPE=Debug

routers: $(CACHE)
	$(CMAKE) --build $(BUILD_DIR) -j $(JOBS) --target bash_router zsh_router

# Builds everything (the test binaries need the libraries anyway), then runs ctest.
test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure -j $(JOBS)

# `make <binary>` builds that one target; with ARGS set it also runs it.
$(BINARIES): $(CACHE)
	$(CMAKE) --build $(BUILD_DIR) -j $(JOBS) --target $@
ifdef ARGS
	./$(BUILD_DIR)/src/cli/$@ $(ARGS)
endif

# Symlink compile_commands.json to the repo root for clangd / IDEs.
compile-commands: $(CACHE)
	ln -sf $(BUILD_DIR)/compile_commands.json compile_commands.json

clean:
	@if [ -f $(CACHE) ]; then $(CMAKE) --build $(BUILD_DIR) --target clean; fi

distclean:
	rm -rf build compile_commands.json

rebuild: distclean
	$(MAKE) --no-print-directory build

info:
	@echo "BUILD_TYPE = $(BUILD_TYPE)"
	@echo "BUILD_DIR  = $(BUILD_DIR)"
	@echo "TORCH_DIR  = $(TORCH_DIR)"
	@echo "CUDACXX    = $(CUDACXX)"
	@echo "GENERATOR  = $(GENERATOR)"
	@echo "JOBS       = $(JOBS)"

help:
	@echo "Targets:"
	@echo "  all / build       configure if needed, then build everything (default)"
	@echo "  release / debug   build with that BUILD_TYPE into build/release or build/debug"
	@echo "  configure         run the CMake configure step only"
	@echo "  reconfigure       drop CMakeCache.txt and configure again"
	@echo "  routers           build bash_router and zsh_router only"
	@echo "  test              build, then run the C++ unit tests with ctest"
	@echo "  <binary>          build one of: $(BINARIES)"
	@echo "                    with ARGS=\"...\" it also runs it, e.g."
	@echo "                    make bash_router ARGS=\"ls -la\""
	@echo "  compile-commands  symlink compile_commands.json into the repo root"
	@echo "  clean             remove build outputs in BUILD_DIR, keep the configuration"
	@echo "  distclean         remove build/ entirely"
	@echo "  rebuild           distclean, then build"
	@echo "  info              print the resolved variables"
	@echo ""
	@echo "Variables: BUILD_TYPE BUILD_DIR TORCH_DIR CUDACXX GENERATOR JOBS CMAKE_ARGS"

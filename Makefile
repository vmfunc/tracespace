# Convenience wrapper around CMake.
#
#   make                 build (Release)
#   make test            build and run the tests
#   make install         install the binary and the Neovim plugin under PREFIX
#   make install-nvim    install, then register the plugin in your Neovim config
#   make uninstall       remove everything the two install targets created
#   make clean           delete the build directory
#
# PREFIX defaults to ~/.local (%LOCALAPPDATA%\tracespace on Windows):
#   make install PREFIX=/usr/local
#
# Recipes are single cmake / nvim commands so they run the same under sh and cmd.exe.

BUILD  ?= build
CONFIG ?= Release
NVIM   ?= nvim

ifeq ($(OS),Windows_NT)
PREFIX ?= $(subst \,/,$(LOCALAPPDATA))/tracespace
else
PREFIX ?= $(HOME)/.local
endif

.PHONY: all build test install install-nvim uninstall clean

all: build

build:
	cmake -S . -B "$(BUILD)" -DCMAKE_BUILD_TYPE=$(CONFIG)
	cmake --build "$(BUILD)" --config $(CONFIG)

test: build
	ctest --test-dir "$(BUILD)" -C $(CONFIG) --output-on-failure

install: build
	cmake --install "$(BUILD)" --config $(CONFIG) --prefix "$(PREFIX)" --component tracespace

install-nvim: install
	$(NVIM) --headless --clean -l scripts/nvim-config.lua install "$(PREFIX)"

uninstall:
	$(NVIM) --headless --clean -l scripts/nvim-config.lua uninstall "$(PREFIX)"

clean:
	cmake -E rm -rf "$(BUILD)"

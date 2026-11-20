#!/bin/sh
# Build and run every test on Linux, inside the container from tests/linux/Dockerfile:
#
#   docker build -t tracespace-linux tests/linux
#   docker run --rm -v "$PWD":/src -v tracespace-build:/build tracespace-linux sh tests/linux/run.sh
#
# Covers what cannot run on a Windows host: inotify, the Unix-socket transport,
# the interactive view driven through a pty, and the Neovim plugin.
set -eu

SRC=${SRC:-/src}
BUILD=${BUILD:-/build}

echo "== configure and build"
cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release >"$BUILD.configure.log" 2>&1 \
    || { tail -40 "$BUILD.configure.log"; exit 1; }
cmake --build "$BUILD" >"$BUILD.build.log" 2>&1 || { grep -E "error|FAILED" -A5 "$BUILD.build.log" | head -80; exit 1; }
grep -E "warning: " "$BUILD.build.log" | grep -v _deps || true

echo "== unit tests"
"$BUILD/tracespace_tests"

echo "== interactive view (pty)"
python3 "$SRC/tests/linux/tui_test.py" "$BUILD/tracespace"

echo "== neovim plugin ($(nvim --version | head -1))"
cd "$SRC"
TRACESPACE_BIN="$BUILD/tracespace" nvim --headless --clean \
    -c "set rtp+=$SRC/nvim" -c "luafile $SRC/tests/nvim/plugin_test.lua"

echo "== all Linux checks passed"

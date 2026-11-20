#!/bin/sh
# Records demo/demo.gif from demo/demo.tape.
#
#   sh demo/record.sh            use vhs, nvim and tracespace from PATH
#   sh demo/record.sh --docker   build and record inside Docker
#
# Run from the repository root. Still frames are written to demo/frames/.
set -eu

if [ "${1:-}" = "--docker" ]; then
    docker build -t tracespace-demo demo
    # MSYS_NO_PATHCONV keeps Git Bash on Windows from rewriting the container paths
    MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W 2>/dev/null || pwd)":/src -v tracespace-demo-build:/build \
        tracespace-demo sh demo/record.sh --inside
    exit 0
fi

if [ "${1:-}" = "--inside" ]; then
    cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DTRACESPACE_BUILD_TESTS=OFF >/build/configure.log 2>&1 \
        || { tail -30 /build/configure.log; exit 1; }
    cmake --build /build >/build/build.log 2>&1 || { tail -30 /build/build.log; exit 1; }
    PATH="/build:$PATH"
    export PATH
fi

for tool in vhs nvim tracespace; do
    command -v "$tool" >/dev/null 2>&1 || { echo "record.sh: $tool not found on PATH" >&2; exit 1; }
done
mkdir -p demo/frames
vhs demo/demo.tape
echo "wrote demo/demo.gif"

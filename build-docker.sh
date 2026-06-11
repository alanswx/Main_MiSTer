#!/bin/bash
# Cross-compile MiSTer Main inside Docker (for building on macOS / Apple Silicon).
#
# First run builds the toolchain image (slow, one time). After that it just
# runs `make` against the current source tree, producing bin/MiSTer.
#
# Usage:
#   ./build-docker.sh           # make
#   ./build-docker.sh clean     # make clean
#   ./build-docker.sh V=1       # verbose, or any other make argument

set -e
set -o pipefail

IMAGE=mister-build
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Build the image if it doesn't exist yet.
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "Building $IMAGE image (one-time toolchain setup)..."
    docker build -t "$IMAGE" "$DIR"
fi

docker run --rm -v "$DIR":/src "$IMAGE" make "$@"

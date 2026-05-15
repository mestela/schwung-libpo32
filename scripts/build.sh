#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$REPO_ROOT"

IMAGE_NAME="schwung-po32-builder"
MODULE_ID="po32-drum"
DIST_DIR="dist/${MODULE_ID}"
TARBALL="dist/${MODULE_ID}-module.tar.gz"

if [ ! -f "/.dockerenv" ]; then
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
    fi
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        ./scripts/build.sh
    exit 0
fi

CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"
CC="${CC:-${CROSS_PREFIX}gcc}"

rm -rf dist build
mkdir -p "$DIST_DIR" build

echo "Compiling DSP..."
"$CC" -Ofast -shared -fPIC \
    -march=armv8-a -mtune=cortex-a72 \
    -fomit-frame-pointer -fno-stack-protector \
    -DNDEBUG \
    -Isrc/dsp \
    src/dsp/po32_drum.c \
    src/dsp/po32.c \
    src/dsp/po32_synth.c \
    src/dsp/po32_patch_import.c \
    -o build/dsp.so \
    -lm

echo "Packaging..."
cp build/dsp.so    "$DIST_DIR/dsp.so"
chmod 0755         "$DIST_DIR/dsp.so"
cp src/module.json "$DIST_DIR/module.json"
cp src/ui.js       "$DIST_DIR/ui.js"

mkdir -p "$DIST_DIR/kits"
cp -r src/kits/. "$DIST_DIR/kits/"

(cd dist && tar -czf "${MODULE_ID}-module.tar.gz" "${MODULE_ID}/")

echo "Built: $TARBALL"
ls -lh "$TARBALL"

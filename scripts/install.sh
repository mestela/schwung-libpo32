#!/usr/bin/env bash
# Install po32-drum module to Move.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$REPO_ROOT"

MODULE_ID="po32-drum"
TARGET="/data/UserData/schwung/modules/sound_generators/${MODULE_ID}"

if [ ! -d "dist/${MODULE_ID}" ]; then
    echo "Error: dist/${MODULE_ID} not found. Run ./scripts/build.sh first."
    exit 1
fi

echo "=== Installing ${MODULE_ID} to Move ==="
ssh ableton@move.local "mkdir -p ${TARGET}"
scp -r "dist/${MODULE_ID}"/. "ableton@move.local:${TARGET}/"
ssh ableton@move.local "chmod -R a+rw ${TARGET}"

echo ""
echo "Installed to: ${TARGET}"
echo "Restart Schwung on the Move to load the module."

#!/bin/bash
# tools/compile.sh — Cross-compile a C test case for riscv64
# Uses native musl-cross if available, falls back to Docker
# Usage: tools/compile.sh test_mmap_flags
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TEST_NAME="${1:?Usage: compile.sh <test_name>}"

SRC="$ROOT_DIR/tests/cases/${TEST_NAME}.c"
if [ ! -f "$SRC" ]; then
    echo "ERROR: $SRC not found" >&2
    exit 1
fi

if command -v riscv64-linux-musl-gcc &>/dev/null; then
    echo "[compile] Using native riscv64-linux-musl-gcc..."
    riscv64-linux-musl-gcc -static -O2 -Wall \
        -I"$ROOT_DIR/tests/cases" \
        -o "$ROOT_DIR/tests/bin/$TEST_NAME" "$SRC"
else
    echo "[compile] Using Docker starry-cross..."
    docker run --rm \
        --platform linux/amd64 \
        -v "$ROOT_DIR/tests/cases:/src" \
        -v "$ROOT_DIR/tests/bin:/out" \
        starry-cross \
        riscv64-linux-musl-gcc -static -O2 -Wall \
            -I/src \
            -o "/out/$TEST_NAME" "/src/${TEST_NAME}.c"
fi

echo "[compile] Output: tests/bin/$TEST_NAME ($(wc -c < "$ROOT_DIR/tests/bin/$TEST_NAME") bytes)"

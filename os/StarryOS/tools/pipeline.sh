#!/bin/bash
# tools/pipeline.sh — Full test pipeline: compile -> inject -> run -> report
# Usage: tools/pipeline.sh <test_name>
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TEST_NAME="${1:?Usage: pipeline.sh <test_name>}"

echo "============================================"
echo "  StarryOS Test Pipeline: $TEST_NAME"
echo "============================================"
echo ""

# Step 1: Compile
echo ">>> Step 1/3: Compile"
"$SCRIPT_DIR/compile.sh" "$TEST_NAME"
echo ""

# Step 2: Inject into rootfs
echo ">>> Step 2/3: Inject into rootfs"
"$SCRIPT_DIR/inject.sh" "$TEST_NAME"
echo ""

# Step 3: Build & Run
echo ">>> Step 3/3: Build, boot, and capture"
"$SCRIPT_DIR/run.sh" "$TEST_NAME"
echo ""

echo "============================================"
echo "  Pipeline complete for: $TEST_NAME"
echo "============================================"

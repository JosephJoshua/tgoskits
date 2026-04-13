#!/bin/bash
# tools/run.sh — Build StarryOS and run it in QEMU, capturing test output
# Usage: tools/run.sh <test_name>
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TEST_NAME="${1:?Usage: run.sh <test_name>}"
RESULT_FILE="$ROOT_DIR/tests/results/${TEST_NAME}.txt"
TIMEOUT_SECS=60
TGOSKITS_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"
KERNEL_ELF="$TGOSKITS_ROOT/target/riscv64gc-unknown-none-elf/release/starryos"
KERNEL_BIN="$ROOT_DIR/tests/bin/starryos.bin"

cd "$ROOT_DIR"

echo "[run] Building StarryOS (via tgoskits workspace)..."
(cd "$TGOSKITS_ROOT" && cargo starry build --arch riscv64) 2>&1 | tail -5

echo "[run] Converting ELF to raw binary..."
rust-objcopy --binary-architecture=riscv64 "$KERNEL_ELF" --strip-all -O binary "$KERNEL_BIN"

echo "[run] Booting QEMU (timeout ${TIMEOUT_SECS}s)..."
timeout "$TIMEOUT_SECS" qemu-system-riscv64 \
    -machine virt \
    -nographic \
    -m 1G \
    -bios default \
    -kernel "$KERNEL_BIN" \
    -device virtio-blk-pci,drive=disk0 \
    -drive id=disk0,if=none,format=raw,file="$ROOT_DIR/make/disk.img" \
    2>&1 | tee "$RESULT_FILE" || true

echo "[run] Parsing results..."

# Extract test lines
PASSES=$(grep -c '^PASS:' "$RESULT_FILE" 2>/dev/null || echo "0")
FAILS=$(grep -c '^FAIL:' "$RESULT_FILE" 2>/dev/null || echo "0")

echo ""
echo "============================="
echo "  Results for: $TEST_NAME"
echo "============================="
grep '^PASS:\|^FAIL:' "$RESULT_FILE" 2>/dev/null || echo "(no test output captured)"
echo "-----------------------------"
echo "  PASS: $PASSES  FAIL: $FAILS"
echo "============================="

# Write JSON result
JSON_FILE="$ROOT_DIR/tests/results/${TEST_NAME}.json"
cat > "$JSON_FILE" <<EOF
{
  "test": "$TEST_NAME",
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "pass": $PASSES,
  "fail": $FAILS,
  "details": [
$(grep '^PASS:\|^FAIL:' "$RESULT_FILE" 2>/dev/null | sed 's/"/\\"/g; s/^/    "/; s/$/"/' | paste -sd',' - || echo '    "(no output)"')
  ]
}
EOF

echo "[run] Results written to $JSON_FILE"

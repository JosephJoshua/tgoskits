#!/bin/bash
# tools/inject.sh — Inject test binary into the StarryOS rootfs ext4 image
# Uses Docker to mount ext4 (macOS can't mount ext4 natively)
# Usage: tools/inject.sh test_mmap_flags
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TEST_NAME="${1:?Usage: inject.sh <test_name>}"
DISK_IMG="$ROOT_DIR/make/disk.img"
TEST_BIN="$ROOT_DIR/tests/bin/$TEST_NAME"

if [ ! -f "$DISK_IMG" ]; then
    echo "ERROR: $DISK_IMG not found. Run 'make rootfs' first." >&2
    exit 1
fi
if [ ! -f "$TEST_BIN" ]; then
    echo "ERROR: $TEST_BIN not found. Run 'tools/compile.sh $TEST_NAME' first." >&2
    exit 1
fi

echo "[inject] Injecting $TEST_NAME into rootfs..."

# Create a test runner script that runs the test and powers off
RUNNER_SCRIPT=$(cat <<'RUNNER'
#!/bin/sh
echo "=== StarryOS Test Runner ==="
/starry_test
EXIT_CODE=$?
echo "=== Test Runner Exit: $EXIT_CODE ==="
poweroff -f
RUNNER
)

# Use Docker to mount the ext4 image and copy files in
docker run --rm --privileged \
    -v "$DISK_IMG:/disk.img" \
    -v "$TEST_BIN:/test_bin:ro" \
    alpine:3.20 sh -c "
        mkdir -p /mnt &&
        mount -o loop /disk.img /mnt &&
        cp /test_bin /mnt/starry_test &&
        chmod 755 /mnt/starry_test &&
        echo '#!/bin/sh
echo \"=== StarryOS Test Runner ===\"
/starry_test
EXIT_CODE=\$?
echo \"=== Test Runner Exit: \$EXIT_CODE ===\"
poweroff -f' > /mnt/test_runner.sh &&
        chmod 755 /mnt/test_runner.sh &&
        sync &&
        umount /mnt &&
        echo '[inject] Done.'
    "

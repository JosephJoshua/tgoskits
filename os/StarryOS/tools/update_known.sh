#!/bin/bash
# tools/update_known.sh — Update tests/known.json with results from a test run
# Usage: tools/update_known.sh <test_name>
# Reads tests/results/<test_name>.json and merges into tests/known.json
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TEST_NAME="${1:?Usage: update_known.sh <test_name>}"
RESULT_FILE="$ROOT_DIR/tests/results/${TEST_NAME}.json"
KNOWN_FILE="$ROOT_DIR/tests/known.json"

if [ ! -f "$RESULT_FILE" ]; then
    echo "ERROR: $RESULT_FILE not found" >&2
    exit 1
fi

# Extract syscall name from test name (test_mmap_flags -> mmap)
SYSCALL=$(echo "$TEST_NAME" | sed 's/^test_//' | sed 's/_[a-z]*$//')

PASS=$(python3 -c "import json; d=json.load(open('$RESULT_FILE')); print(d.get('pass',0))")
FAIL=$(python3 -c "import json; d=json.load(open('$RESULT_FILE')); print(d.get('fail',0))")
TIMESTAMP=$(python3 -c "import json; d=json.load(open('$RESULT_FILE')); print(d.get('timestamp',''))")

# Get failed test names
FAILURES=$(grep '^FAIL:' "$ROOT_DIR/tests/results/${TEST_NAME}.txt" 2>/dev/null | sed 's/^FAIL: //' || echo "")

python3 -c "
import json, sys

known_path = '$KNOWN_FILE'
with open(known_path) as f:
    known = json.load(f)

syscall = '$SYSCALL'
if syscall not in known['syscalls']:
    known['syscalls'][syscall] = {'tested': [], 'bugs_found': [], 'last_run': '', 'pass_rate': ''}

entry = known['syscalls'][syscall]
entry['last_run'] = '$TIMESTAMP'
entry['pass_rate'] = '${PASS}/${PASS}+${FAIL}'.replace('+', ' + ').strip()

# Add test name if not already tracked
if '$TEST_NAME' not in entry['tested']:
    entry['tested'].append('$TEST_NAME')

with open(known_path, 'w') as f:
    json.dump(known, f, indent=2)

print(f'[update_known] Updated {syscall}: pass=$PASS fail=$FAIL')
"

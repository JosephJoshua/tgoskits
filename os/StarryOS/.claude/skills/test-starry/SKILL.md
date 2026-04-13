---
name: test-starry
description: Run automated syscall tests against StarryOS. Use when user says "test starry", "test syscall", "test mmap", "find bugs", "discover vulnerabilities", or "/test-starry". Generates C test cases, cross-compiles, injects into rootfs, boots QEMU, and reports PASS/FAIL results.
---

# StarryOS Automated Syscall Testing

You are running the StarryOS automated test pipeline. Follow this workflow precisely.

## Arguments

The user may provide:
- A specific syscall name: `/test-starry mmap` → test mmap
- "discover" or no argument: `/test-starry discover` → auto-select targets
- A specific test file: `/test-starry test_pwritev2` → run existing test

## Workflow

### Step 1: Select Target

If "discover" mode or no specific target:
1. Read `tests/known.json` to see what's already tested
2. Read `kernel/src/syscall/mod.rs` for all implemented syscalls
3. Grep for `TODO|FIXME` in `kernel/src/` to find weak spots
4. Rank by: has TODO > untested > complex implementation
5. Present top 5 targets to the user with reasoning

If specific syscall given:
1. Read the implementation in `kernel/src/syscall/` for that syscall
2. Identify edge cases, flag combinations, error paths
3. Proceed to Step 2

### Step 2: Generate Test

1. Create `tests/cases/test_<name>.c` using the harness:
```c
#include "starry_test.h"
// Additional headers as needed

TEST_BEGIN("suite_name")

TEST("test_name") {
    // Test body — use EXPECT_EQ, EXPECT_TRUE, EXPECT_ERRNO, EXPECT_OK
} TEND

TEST_END
```

2. Important rules:
   - Use `TEST("name") { ... } TEND` syntax (NOT `TEST("name", { ... })`)
   - Initialize struct members one-by-one (NOT with designated initializers in the macro body)
   - For syscalls not in musl libc, use `syscall(SYS_xxx, ...)` directly
   - Always include positive (should-work) AND negative (should-fail) tests
   - Clear errno before each test (done automatically by TEST macro)

3. Show the generated code to the user and explain what each test checks.

### Step 3: Compile

Run:
```bash
tools/compile.sh test_<name>
```

If compilation fails, fix the C code and retry.

### Step 4: Inject

Run:
```bash
tools/inject.sh test_<name>
```

This mounts the rootfs ext4 image via Docker, copies the binary as `/starry_test`, and creates `/test_runner.sh`.

### Step 5: Run

Run:
```bash
tools/run.sh test_<name>
```

This builds the kernel (via parent workspace `cargo starry build --arch riscv64`), converts to raw binary, and boots QEMU with a 60-second timeout.

**Alternative (if kernel already built and just re-running with different test):**
```bash
tools/inject.sh test_<name>
timeout 60 qemu-system-riscv64 -machine virt -nographic -m 1G -bios default \
  -kernel tests/bin/starryos.bin \
  -device virtio-blk-pci,drive=disk0 \
  -drive id=disk0,if=none,format=raw,file=make/disk.img
```

### Step 6: Analyze Results

1. Parse `tests/results/test_<name>.txt` for PASS/FAIL lines
2. For each FAIL:
   - Read the relevant kernel source code
   - Identify the root cause
   - Propose a fix (show the code diff)
3. Update `tests/known.json` with results
4. Report summary to user

### Step 7: Iterate (if user wants)

- "fix it" → Apply the proposed patch, rebuild, re-run
- "test more" → Go back to Step 1 with discover mode
- "next" → Pick next priority target

## Key Facts

- Kernel build MUST happen from `../../` (tgoskits workspace root): `cd ../.. && cargo starry build --arch riscv64`
- Cross-compiler: `riscv64-linux-musl-gcc` (native macOS via brew musl-cross)
- QEMU needs: `-bios default -device virtio-blk-pci` (PCI bus, not MMIO)
- Test output format: `PASS: suite::name` or `FAIL: suite::name (details)`
- init.sh auto-detects `/test_runner.sh` in rootfs and runs it instead of shell
- The rootfs is at `make/disk.img` (ext4, 1GB)
- If kernel is already built, you can skip rebuild and just inject + QEMU boot

# StarryOS Development Guide

## Project Structure
- `kernel/src/` — StarryOS kernel source (syscall handlers, task management, memory, pseudofs)
- `starryos/src/` — Binary entry point
- `make/` — Build system (Makefile-based)
- `tools/` — Automated test pipeline scripts
- `tests/` — Test cases, binaries, and results

## Build
This project MUST be built from the parent workspace (`../../`):
```bash
cd ../.. && cargo starry build --arch riscv64
```
Direct `make build` from this directory will FAIL due to version/registry issues.

## Test Pipeline (`/test-starry` workflow)

### Quick Reference
```bash
# Full pipeline for a single test:
tools/compile.sh <test_name>    # Cross-compile C test → tests/bin/
tools/inject.sh <test_name>     # Inject into rootfs (needs Docker for ext4 mount)
tools/run.sh <test_name>        # Build kernel + QEMU boot + capture results

# Or all-in-one:
tools/pipeline.sh <test_name>
```

### Writing New Tests
1. Create `tests/cases/test_<name>.c` using the `starry_test.h` harness
2. Use `TEST("name") { ... } TEND` syntax (avoids C preprocessor comma issues)
3. Use `EXPECT_EQ`, `EXPECT_TRUE`, `EXPECT_ERRNO`, `EXPECT_OK` macros
4. Each test prints `PASS: suite::name` or `FAIL: suite::name (details)` to stdout

### QEMU Boot (manual)
```bash
qemu-system-riscv64 -machine virt -nographic -m 1G -bios default \
  -kernel tests/bin/starryos.bin \
  -device virtio-blk-pci,drive=disk0 \
  -drive id=disk0,if=none,format=raw,file=make/disk.img
```

### Requirements
- `riscv64-linux-musl-gcc` (via `brew install FiloSottile/musl-cross/musl-cross --with-riscv64`)
- `qemu-system-riscv64` (via homebrew)
- `ax-config-gen` (symlinked from `axconfig-gen`)
- Docker (for rootfs ext4 mounting on macOS)

## Known Bugs Found
- **pwritev/pwritev2**: `kernel/src/syscall/fs/io.rs:220` calls `read_at` instead of `write_at` (copy-paste from preadv2). Confirmed via automated test.

## Key Source Locations
- Syscall dispatch: `kernel/src/syscall/mod.rs` (210 Sysno variants)
- Scheduler: Round-Robin via ax-task `sched-rr` feature, per-CPU queues with `smp` feature
- Page fault: `kernel/src/task/user.rs:32-41`
- ELF loader: `kernel/src/mm/loader.rs`
- Signal handling: `kernel/src/task/signal.rs`

FROM alpine:3.20

# Build essentials
RUN apk add --no-cache \
    build-base curl git bash wget tar xz python3 perl linux-headers

# Rust nightly
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | \
    sh -s -- -y --default-toolchain nightly-2026-04-01 \
    --component rust-src llvm-tools rustfmt clippy \
    --target riscv64gc-unknown-none-elf aarch64-unknown-none-softfloat \
           loongarch64-unknown-none-softfloat x86_64-unknown-none
ENV PATH="/root/.cargo/bin:$PATH"

# Install cargo tools needed by StarryOS build system
RUN cargo install cargo-axplat cargo-binutils axconfig-gen && \
    ln -s $(which axconfig-gen) /root/.cargo/bin/ax-config-gen

# musl cross-compiler for riscv64 (needed by lwext4_rust)
RUN wget -qO- https://musl.cc/riscv64-linux-musl-cross.tgz | tar xz -C /opt
ENV PATH="/opt/riscv64-linux-musl-cross/bin:$PATH"

WORKDIR /workspace

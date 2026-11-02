# Cross-build environment for the gtelwifi mainline kernel.
# Native arm64 image on Apple Silicon; the kernel tree lives in a named
# volume (case-sensitive ext4), only the project dir is bind-mounted.
FROM debian:trixie-slim

RUN dpkg --add-architecture armhf && apt-get update \
    && apt-get install -y --no-install-recommends \
        gcc-arm-linux-gnueabihf libc6-dev-armhf-cross binutils-arm-linux-gnueabihf \
        build-essential bc bison flex libssl-dev libelf-dev \
        device-tree-compiler cpio kmod python3 git ca-certificates \
        qemu-system-arm busybox-static:armhf dropbear-bin:armhf e2fsprogs \
        clang lld llvm \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work

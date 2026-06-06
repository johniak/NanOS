# NanOS build environment.
# Builds an i686-elf cross-toolchain from source and bundles every tool needed
# to compile the kernel and assemble the bootable GRUB2 ext2 image.
# QEMU is intentionally NOT here: builds run in this container, testing runs
# natively on the host.
FROM debian:trixie-slim

# Pinned toolchain versions for reproducibility.
ARG BINUTILS_VERSION=2.43
ARG GCC_VERSION=14.2.0
ARG TARGET=i686-elf
ARG PREFIX=/opt/cross

ENV PATH="${PREFIX}/bin:${PATH}"

# Runtime + build dependencies.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates wget make \
        build-essential bison flex texinfo \
        libgmp-dev libmpc-dev libmpfr-dev \
        nasm grub-pc-bin grub-common xorriso mtools \
        e2fsprogs parted \
    && rm -rf /var/lib/apt/lists/*

# Build binutils, then gcc (all-gcc + libgcc only — freestanding kernel).
RUN set -eux; \
    cd /tmp; \
    wget -q "https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VERSION}.tar.xz"; \
    wget -q "https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VERSION}/gcc-${GCC_VERSION}.tar.xz"; \
    tar xf "binutils-${BINUTILS_VERSION}.tar.xz"; \
    tar xf "gcc-${GCC_VERSION}.tar.xz"; \
    mkdir build-binutils; cd build-binutils; \
    ../binutils-${BINUTILS_VERSION}/configure \
        --target="${TARGET}" --prefix="${PREFIX}" \
        --with-sysroot --disable-nls --disable-werror; \
    make -j"$(nproc)"; make install; \
    mkdir /tmp/build-gcc; cd /tmp/build-gcc; \
    ../gcc-${GCC_VERSION}/configure \
        --target="${TARGET}" --prefix="${PREFIX}" \
        --disable-nls --enable-languages=c,c++ --without-headers; \
    make -j"$(nproc)" all-gcc all-target-libgcc; \
    make install-gcc install-target-libgcc; \
    cd /; rm -rf /tmp/*

# Host-side test tooling (separate layer so it never invalidates the toolchain
# build cache above). gcov ships with gcc; lcov gives us coverage reports.
RUN apt-get update && apt-get install -y --no-install-recommends lcov \
    && rm -rf /var/lib/apt/lists/*

# Marker the Makefile checks to know it is running inside the build container.
RUN touch /etc/nanos-build

WORKDIR /src

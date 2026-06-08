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

# Runtime + build dependencies. grub-pc-bin (the i386-pc BIOS GRUB) is amd64/i386 only,
# so it's fetched separately below (its files are target data, not host binaries).
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates wget make \
        build-essential bison flex texinfo \
        libgmp-dev libmpc-dev libmpfr-dev \
        nasm grub-common xorriso mtools \
        e2fsprogs parted \
    && rm -rf /var/lib/apt/lists/*

# The i386-pc GRUB modules (boot.img + *.mod) live in grub-pc-bin, which Debian only
# packages for amd64/i386 — but the files are i386 target data that grub-common's
# grub-mkimage uses on ANY host. Fetch the amd64 .deb and drop its /usr/lib/grub/i386-pc
# into place so the bootable image builds whether the container is amd64 or arm64.
RUN set -eux; cd /tmp; \
    dpkg --add-architecture amd64; \
    apt-get update; \
    apt-get download grub-pc-bin:amd64; \
    dpkg-deb -x grub-pc-bin*.deb /tmp/grubpc; \
    mkdir -p /usr/lib/grub; \
    cp -r /tmp/grubpc/usr/lib/grub/i386-pc /usr/lib/grub/; \
    rm -rf /tmp/grubpc grub-pc-bin*.deb /var/lib/apt/lists/*

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

# picolibc: a C library for the NanOS userland. Built as a SEPARATE static library
# against the cross-gcc above (NOT a toolchain rebuild), so this trailing layer
# never invalidates the expensive toolchain cache. Programs link libc.a + our thin
# syscall glue; sbase cat/ls compile against picolibc's headers.
ARG PICOLIBC_VERSION=1.8.6
RUN apt-get update && apt-get install -y --no-install-recommends \
        meson ninja-build python3 git \
    && rm -rf /var/lib/apt/lists/*
COPY picolibc-i686-elf.txt /tmp/picolibc-i686-elf.txt
RUN set -eux; \
    git clone --depth 1 --branch "${PICOLIBC_VERSION}" \
        https://github.com/picolibc/picolibc /tmp/picolibc; \
    cd /tmp/picolibc; \
    meson setup build --cross-file /tmp/picolibc-i686-elf.txt \
        -Dprefix=/opt/picolibc/i686-elf \
        -Dincludedir=include -Dlibdir=lib \
        -Dmultilib=false -Dpicocrt=false -Dpicolib=true -Dsemihost=false \
        -Dposix-console=true \
        -Dtests=false -Dformat-default=integer -Dthread-local-storage=false; \
    ninja -C build; \
    ninja -C build install; \
    cd /; rm -rf /tmp/picolibc /tmp/picolibc-i686-elf.txt

# Marker the Makefile checks to know it is running inside the build container.
RUN touch /etc/nanos-build

WORKDIR /src

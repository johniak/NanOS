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
        -Dtests=false -Dformat-default=double -Dthread-local-storage=false \
        -Derrno-function=__errno_location; \
    ninja -C build; \
    ninja -C build install; \
    cd /; rm -rf /tmp/picolibc /tmp/picolibc-i686-elf.txt

# Rust toolchain (trailing layer — never invalidates the C toolchain cache). For writing
# NanOS apps in Rust against the C-ABI libnwui. We compile for a CUSTOM bare i686 target with
# no SIMD + soft-float (matching the C userland / the emulated CPU), which needs nightly +
# rust-src to build core/alloc for it (-Z build-std).
ENV RUSTUP_HOME=/opt/rustup CARGO_HOME=/opt/cargo
ENV PATH="/opt/cargo/bin:${PATH}"
RUN set -eux; \
    wget -q -O /tmp/rustup.sh https://sh.rustup.rs; \
    sh /tmp/rustup.sh -y --default-toolchain nightly --profile minimal --component rust-src; \
    rm /tmp/rustup.sh; \
    rustc --version

# Second cross-toolchain: x86_64-elf (the long-mode port target). Built from the SAME
# pinned binutils/gcc sources as i686-elf, into the same PREFIX (the triple-prefixed
# binaries don't collide: x86_64-elf-gcc vs i686-elf-gcc). Appended as a trailing layer
# so it never invalidates the cached i686-elf toolchain above. ~20-40 min first build
# (amd64 emulated on Apple Silicon), then cached.
ARG TARGET64=x86_64-elf
RUN set -eux; \
    cd /tmp; \
    wget -q "https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VERSION}.tar.xz"; \
    wget -q "https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VERSION}/gcc-${GCC_VERSION}.tar.xz"; \
    tar xf "binutils-${BINUTILS_VERSION}.tar.xz"; \
    tar xf "gcc-${GCC_VERSION}.tar.xz"; \
    mkdir build-binutils64; cd build-binutils64; \
    ../binutils-${BINUTILS_VERSION}/configure \
        --target="${TARGET64}" --prefix="${PREFIX}" \
        --with-sysroot --disable-nls --disable-werror; \
    make -j"$(nproc)"; make install; \
    mkdir /tmp/build-gcc64; cd /tmp/build-gcc64; \
    ../gcc-${GCC_VERSION}/configure \
        --target="${TARGET64}" --prefix="${PREFIX}" \
        --disable-nls --enable-languages=c,c++ --without-headers \
        --disable-libssp --disable-libsanitizer; \
    make -j"$(nproc)" all-gcc all-target-libgcc; \
    make install-gcc install-target-libgcc; \
    cd /; rm -rf /tmp/*

# Marker the Makefile checks to know it is running inside the build container.
RUN touch /etc/nanos-build

# x86_64-elf picolibc: the 64-bit counterpart of the i686-elf picolibc above, built
# against the x86_64-elf cross-gcc for the long-mode port's libc.ndl. Trailing layer
# (after both toolchains + the marker) so it never invalidates the expensive cached
# toolchain layers. meson/ninja/git/python3 were already installed by the i686 picolibc
# layer. Built non-PIC / small code model (-fno-pic -mcmodel=small -mno-red-zone via the
# cross-file) so objects emit absolute relocs that mknx64 handles, not GOTPCREL/PLT.
COPY picolibc-x86_64-elf.txt /tmp/picolibc-x86_64-elf.txt
RUN set -eux; \
    git clone --depth 1 --branch "${PICOLIBC_VERSION}" \
        https://github.com/picolibc/picolibc /tmp/picolibc; \
    cd /tmp/picolibc; \
    meson setup build --cross-file /tmp/picolibc-x86_64-elf.txt \
        -Dprefix=/opt/picolibc/x86_64-elf \
        -Dincludedir=include -Dlibdir=lib \
        -Dmultilib=false -Dpicocrt=false -Dpicolib=true -Dsemihost=false \
        -Dposix-console=true \
        -Dtests=false -Dformat-default=double -Dthread-local-storage=false \
        -Derrno-function=__errno_location; \
    ninja -C build; \
    ninja -C build install; \
    cd /; rm -rf /tmp/picolibc /tmp/picolibc-x86_64-elf.txt

WORKDIR /src

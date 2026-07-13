# NanOS build environment.
# Builds the x86_64-elf cross-toolchain from source and bundles every tool needed
# to compile the kernel and assemble the bootable disk image (hybrid GPT + Limine).
# QEMU is intentionally NOT here: builds run in this container, testing runs
# natively on the host.
# (The i686-elf toolchain + 32-bit picolibc layers were retired with the i686 flow,
# docs/superpowers/plans/2026-07-13-i686-retirement.md.)
FROM debian:trixie-slim

# Pinned toolchain versions for reproducibility.
ARG BINUTILS_VERSION=2.43
ARG GCC_VERSION=14.2.0
ARG TARGET=x86_64-elf
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
# grub-mkimage uses on ANY host. grub-mkrescue needs them for the bringup64 rescue ISO
# (a GRUB BIOS-boot implementation detail — unrelated to the retired i686 NanOS flow).
RUN set -eux; cd /tmp; \
    dpkg --add-architecture amd64; \
    apt-get update; \
    apt-get download grub-pc-bin:amd64; \
    dpkg-deb -x grub-pc-bin*.deb /tmp/grubpc; \
    mkdir -p /usr/lib/grub; \
    cp -r /tmp/grubpc/usr/lib/grub/i386-pc /usr/lib/grub/; \
    rm -rf /tmp/grubpc grub-pc-bin*.deb /var/lib/apt/lists/*

# Build binutils, then gcc (all-gcc + libgcc only — freestanding kernel).
# ~20-40 min first build (amd64 emulated on Apple Silicon), then cached.
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
        --disable-nls --enable-languages=c,c++ --without-headers \
        --disable-libssp --disable-libsanitizer; \
    make -j"$(nproc)" all-gcc all-target-libgcc; \
    make install-gcc install-target-libgcc; \
    cd /; rm -rf /tmp/*

# Host-side test tooling (separate layer so it never invalidates the toolchain
# build cache above). gcov ships with gcc; lcov gives us coverage reports.
RUN apt-get update && apt-get install -y --no-install-recommends lcov \
    && rm -rf /var/lib/apt/lists/*

# Marker the Makefile checks to know it is running inside the build container.
RUN touch /etc/nanos-build

# picolibc: the C library for the NanOS userland. Built as a SEPARATE static library
# against the cross-gcc above (NOT a toolchain rebuild), so this trailing layer never
# invalidates the expensive toolchain cache. Programs link libc.a + our thin syscall
# glue. Built non-PIC / small code model (-fno-pic -mcmodel=small -mno-red-zone via the
# cross-file) so objects emit absolute relocs that mknx64 handles, not GOTPCREL/PLT.
ARG PICOLIBC_VERSION=1.8.6
RUN apt-get update && apt-get install -y --no-install-recommends \
        meson ninja-build python3 git \
    && rm -rf /var/lib/apt/lists/*
COPY picolibc-x86_64-elf.txt /tmp/picolibc-x86_64-elf.txt
# Backport picolibc's upstream malloc 16-byte alignment fix to 1.8.6: nano-malloc derives
# MALLOC_CHUNK_ALIGN from offsetof(align_chunk_t, u), whose union {void*,double,long long,size_t}
# aligns to 8 on x86_64 — but the System V AMD64 ABI requires 16 (max_align_t). Widening the union
# with `long double` (alignof 16 here) raises the alignment to 16 so malloc/calloc/realloc satisfy
# 16-byte SSE stores (e.g. Mesa ralloc's `movaps %xmm0,(%rax)`).
COPY picolibc-nano-malloc-align16.patch /tmp/picolibc-nano-malloc-align16.patch
RUN set -eux; \
    git clone --depth 1 --branch "${PICOLIBC_VERSION}" \
        https://github.com/picolibc/picolibc /tmp/picolibc; \
    cd /tmp/picolibc; \
    git apply /tmp/picolibc-nano-malloc-align16.patch; \
    meson setup build --cross-file /tmp/picolibc-x86_64-elf.txt \
        -Dprefix=/opt/picolibc/x86_64-elf \
        -Dincludedir=include -Dlibdir=lib \
        -Dmultilib=false -Dpicocrt=false -Dpicolib=true -Dsemihost=false \
        -Dposix-console=true \
        -Dtests=false -Dformat-default=double -Dthread-local-storage=false \
        -Derrno-function=__errno_location; \
    ninja -C build; \
    ninja -C build install; \
    cd /; rm -rf /tmp/picolibc /tmp/picolibc-x86_64-elf.txt /tmp/picolibc-nano-malloc-align16.patch

# Rust toolchain (trailing layer — never invalidates the C toolchain cache). For writing
# NanOS apps in Rust against the C-ABI libnwui (rsexp/libnwui-rs). We compile for the CUSTOM
# bare x86_64-nanos target (user/rust/x86_64-nanos.json), which needs nightly + rust-src to
# build core/alloc for it (-Z build-std).
ENV RUSTUP_HOME=/opt/rustup CARGO_HOME=/opt/cargo
ENV PATH="/opt/cargo/bin:${PATH}"
RUN set -eux; \
    wget -q -O /tmp/rustup.sh https://sh.rustup.rs; \
    sh /tmp/rustup.sh -y --default-toolchain nightly --profile minimal --component rust-src; \
    rm /tmp/rustup.sh; \
    rustc --version

# Limine bootloader (BIOS+UEFI) for the hybrid GPT image. The vN.x-binary branch ships prebuilt
# BIOS stages + BOOTX64.EFI and a tiny host `limine` deploy tool we build with the host cc. Fetched
# via wget+tar (no git in the image). Trailing layer so the toolchain cache above stays valid.
RUN set -eux; cd /tmp; \
    wget -qO limine.tar.gz https://github.com/limine-bootloader/limine/archive/refs/heads/v8.x-binary.tar.gz; \
    d="$(tar tzf limine.tar.gz | head -1 | cut -d/ -f1)"; \
    tar xf limine.tar.gz; \
    cd "$d"; \
    make; \
    install -Dm644 BOOTX64.EFI     /usr/local/share/limine/BOOTX64.EFI; \
    install -Dm644 limine-bios.sys /usr/local/share/limine/limine-bios.sys; \
    install -Dm755 limine           /usr/local/bin/limine; \
    cd /; rm -rf /tmp/limine* /tmp/"$d"

WORKDIR /src

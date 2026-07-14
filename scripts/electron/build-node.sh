#!/usr/bin/env bash
# build-node.sh — cross-configure + build node.nxe for NanOS x86_64 (plan 02 Tasks 2.2/2.3).
#
# Runs INSIDE the nanos-sdk-dev container (the aarch64->x86_64-nanos cross toolchain runs natively
# there). The Makefile `node` target refreshes the x86_64-nanos sysroot from the live tree and mounts
# the repo (/src), toolchain (/work/toolchain) and node checkout (/work/node) before calling this.
#
# This script is the RECORD of the build (plan 02 Decisions): every flag and workaround is here or in
# ports/node/patches/. Compiler flags the nanos toolchain rejects are stripped by the ports/node/nx-*
# wrappers (documented in docs/en/node-port.md), not by editing gyp.
set -eu

NODE_SRC="${NODE_SRC:-/work/node}"
REPO="${REPO:-/src}"
JOBS="${JOBS:-8}"

# Target (nanos) compilers go through the flag-stripping wrappers (each hardcodes its base compiler);
# host tools stay native.
WRAP="$REPO/ports/node"
export CC="$WRAP/nx-node-cc" CXX="$WRAP/nx-node-cxx" AR=x86_64-nanos-ar
export CC_host=cc CXX_host=c++

# NanOS user ABI: fixed-base ET_EXEC, small code model, no red zone, and the __NANOS__ guard for the
# port's #ifdef'd patches. -include nx-dllimport.h routes picolibc's stdio/errno DATA refs through the
# IAT slots mknx needs (same shim the vim/htop ports force-include).
#
# -D__linux__ completes the "NanOS is close enough to linux" decision (plan 02): gyp already selects
# the linux source files (--dest-os=linux), but the picolibc-based nanos gcc does not advertise
# __linux__, so the `#ifdef __linux__` blocks INSIDE those files (abseil ABSL_HAVE_MMAP, V8 platform
# selection, libuv) stay dark and break — e.g. abseil poison.cc references a `data` var only declared
# under ABSL_HAVE_MMAP. Making the compiler agree with gyp's dest-os is the coherent choice; picolibc
# itself is OS-agnostic and does not key off __linux__. NanOS-specific divergence stays behind
# __NANOS__ (which wins where the two differ). See docs/en/node-port.md.
NANOS_CFLAGS="-D__NANOS__ -D__linux__ -fno-pie -mcmodel=small -mno-red-zone -include nx-dllimport.h"
# -g0 wins over gyp's -g: V8's DWARF debug info is enormous (a debug build of V8 is 12+ GiB of
# objects). The first bring-up doesn't need symbols; drop them to keep the build within disk.
NANOS_CFLAGS="$NANOS_CFLAGS -g0"
# picolibc's <limits.h> omits a few POSIX limits: _POSIX_SEM_VALUE_MAX (libstdc++ C++20 <semaphore>
# static_assert) and SSIZE_MAX (libuv strscpy). __LONG_MAX__ is the gcc builtin (arch-safe).
NANOS_CFLAGS="$NANOS_CFLAGS -D_POSIX_SEM_VALUE_MAX=32767 -DSSIZE_MAX=__LONG_MAX__"
# _GNU_SOURCE raises picolibc's __POSIX_VISIBLE so the full POSIX/glibc surface is DECLARED (e.g.
# wcslen, which <cwchar> then imports into std:: — abseil uses std::wcslen). Matches --dest-os=linux;
# openssl already builds with it. Fixes a class of "not declared / not a member of std" visibility
# gaps in one place rather than per call site. __LINUX_ERRNO_EXTENSIONS__ turns on picolibc's
# Linux-only errno values (EHOSTDOWN, ...) that c-ares/node map.
NANOS_CFLAGS="$NANOS_CFLAGS -D_GNU_SOURCE -D__LINUX_ERRNO_EXTENSIONS__"
# __TM_GMTOFF/__TM_ZONE add the BSD/glibc `tm_gmtoff`/`tm_zone` members to picolibc's struct tm
# (gated behind these macros). abseil cctz's time_zone_libc SFINAE requires one of them to exist. The
# libc's localtime_r doesn't populate them, so libc-derived local UTC offset reads 0 — fine on NanOS
# (no real timezone DB; cctz's own tz path is primary anyway).
NANOS_CFLAGS="$NANOS_CFLAGS -D__TM_GMTOFF=tm_gmtoff -D__TM_ZONE=tm_zone"
# googletest (V8's test framework — not linked into node itself): picolibc omits _POSIX_PATH_MAX, and
# gtest's death tests use clone(2). Supply the constant and force the fork-based death-test path.
NANOS_CFLAGS="$NANOS_CFLAGS -D_POSIX_PATH_MAX=256 -DGTEST_HAS_CLONE=0"
# Force-include the port compat header for constants that live in glibc's <signal.h> siginfo section
# but not picolibc's (V8's crash handler switches on them). Kept out of the sysroot signal.h so we
# don't fork picolibc's header.
NANOS_CFLAGS="$NANOS_CFLAGS -include $REPO/ports/node/nanos-node-compat.h"

cd "$NODE_SRC"

# Apply any ports/node/patches/*.patch (idempotent: skip already-applied).
if [ -d "$WRAP/patches" ]; then
	for p in "$WRAP/patches"/*.patch; do
		[ -e "$p" ] || continue
		if git apply --check "$p" 2>/dev/null; then git apply "$p" && echo "applied $(basename "$p")"; \
		else echo "skip $(basename "$p") (already applied or N/A)"; fi
	done
fi

if [ "${SKIP_CONFIGURE:-0}" != 1 ]; then
	python3 configure --dest-os=linux --dest-cpu=x64 --cross-compiling \
		--without-intl --without-inspector --without-node-snapshot \
		--without-npm --without-corepack --openssl-no-asm
fi

CFLAGS="$NANOS_CFLAGS" CXXFLAGS="$NANOS_CFLAGS" make -j"$JOBS"

echo "build-node: produced $(ls -la "$NODE_SRC/out/Release/node" 2>/dev/null || echo '(no node binary yet)')"

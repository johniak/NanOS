#!/bin/sh
# nx-port-build.sh — cross-build an external app (grep / bzip2 / vim) for NanOS, inside the
# nanos-build container, using the nx-gcc wrapper (picolibc + libc-glue + NanOS .nxe link).
#
# This is the arch-aware build driver the NanOS `make grep|vim|bzip2` targets invoke when
# ARCH=x86_64 (the i686 targets keep copying their hand-built .nxe, so i686 is untouched).
# It mirrors the bash fork's build.sh, generalised over a handful of apps. The per-app recipe
# (build system + configure flags) is the only app-specific knowledge; everything else — the
# nx-gcc CC, the LP64 cache fix, mknx — is shared.
#
# Mounts (set up by the Makefile docker run):
#   /src        the NanOS repo (cross artifacts: crt0/nxhdr/libc.ndl.a/mknx, libc-glue headers)
#   /work/src   the app's upstream source tree (e.g. $(SDK_WORK)/grep-3.11)
# The produced <app>.nxe is written back into /work/src so the Makefile can copy it to bin/.
#
# Env (the Makefile sets these for x86_64; the defaults are the i686 values, unused here since
# i686 copies a prebuilt .nxe — but kept so the script is correct if ever run for i686):
#   NX_HOST       configure --host triple  (x86_64-elf)
#   NX_PICO       picolibc prefix          (/opt/picolibc/x86_64-elf)
#   NX_CC         the cross gcc            (x86_64-elf-gcc)
#   NX_ARCHFLAGS  extra codegen flags      (-mcmodel=small -mno-red-zone)
#   NX_LDSCRIPT   the .nxe linker script   (/src/arch/x86_64/user-nx.ld)
#   NX_MKNX       format-correct mknx      (/src/bin/mknx64)
#   NX_LP64       1 -> append the 64-bit sizeof answers to the autoconf cache
set -e

APP="${1:?usage: nx-port-build.sh <app> [srcdir]}"
SRCDIR="${2:-/work/src}"
HERE=/src/scripts

export NANOS=/src
export CC="$HERE/nx-gcc"
export PICO="${NX_PICO:-/opt/picolibc/i686-elf}"
export NX_CC NX_ARCHFLAGS NX_LDSCRIPT
HOST_TRIPLE="${NX_HOST:-i686-elf}"
MKNX="${NX_MKNX:-/src/bin/mknx}"

# Build from a copy on the container's case-sensitive fs (dodges the macOS case trap), exactly
# like the bash fork does. The copy also keeps the mounted source tree pristine.
STAGE="/tmp/port-$APP"
echo "== staging $APP -> $STAGE =="
rm -rf "$STAGE"
cp -a "$SRCDIR" "$STAGE"
cd "$STAGE"

# Seed autoconf cache: the generic cross answers (sizeofs etc.) + LP64 override for x86_64.
seed_cache() {
	cat > "$STAGE/nx.cache" <<-'EOF'
	ac_cv_sizeof_char=1
	ac_cv_sizeof_short=2
	ac_cv_sizeof_int=4
	ac_cv_sizeof_long=4
	ac_cv_sizeof_long_long=8
	ac_cv_sizeof_void_p=4
	ac_cv_sizeof_size_t=4
	ac_cv_sizeof_off_t=8
	ac_cv_c_bigendian=no
	ac_cv_func_mmap_fixed_mapped=yes
	ac_cv_func_malloc_0_nonnull=yes
	ac_cv_func_realloc_0_nonnull=yes
	EOF
	# LP64: long/pointer/size_t are 8 bytes. AC_CHECK_SIZEOF is a compile probe (correct under
	# cross) but the seeded 32-bit values would mask it, so append the right answers last.
	if [ "${NX_LP64:-0}" = 1 ]; then
		printf 'ac_cv_sizeof_long=8\nac_cv_sizeof_void_p=8\nac_cv_sizeof_size_t=8\n' >> "$STAGE/nx.cache"
	fi
}

case "$APP" in
bzip2)
	# Plain Makefile: just build the bzip2 front-end + libbz2 with our CC. (No bzip2recover /
	# self-test, which would try to RUN the cross binary.) Scrub any prebuilt objects first so
	# the upstream tree's stale .o/.a never get linked instead of a freshly cross-built one.
	rm -f *.o libbz2.a bzip2 bzip2recover
	make CC="$CC" libbz2.a
	make CC="$CC" bzip2
	BIN="$STAGE/bzip2"
	;;
grep)
	# gnulib brings its own POSIX header surface; skip nx-gcc's bash-oriented compat force-includes.
	export NX_GNULIB=1
	# Give the (bare) picolibc headers the hosted-POSIX surface gnulib probes for — lstat,
	# getrlimit/RLIMIT_*, getprogname, the *_unlocked stdio family, etc. Idempotent; runs on the
	# container's ephemeral /opt copy only. This is the same adaptation the SDK i686 sysroot got.
	sh "$HERE/posix-hosted-patch.sh" "$PICO/include"
	# Reuse the committed i686 cross cache: 553 gnulib answers (func/decl existence, struct
	# quirks) that depend only on the libc — identical across arches since both link the same
	# NanOS libc.ndl. Drop the arch-specific sizeofs + the pinned compiler/host triple, then
	# append the LP64 sizes. (Far more robust than re-probing each gnulib module from scratch.)
	if [ -f config.cache ]; then
		grep -vE 'alias|ac_cv_env_|ac_cv_prog_CC=|ac_cv_prog_CPP|ac_cv_prog_AWK|i686-nanos|ac_cv_build=|ac_cv_host=|ac_cv_sizeof_long=|ac_cv_sizeof_void_p=|ac_cv_sizeof_size_t=' config.cache > "$STAGE/nx.cache"
	else
		seed_cache
	fi
	printf 'ac_cv_sizeof_long=8\nac_cv_sizeof_void_p=8\nac_cv_sizeof_size_t=8\n' >> "$STAGE/nx.cache"
	# Scrub the upstream tree's stale i686 autotools state (it was last configured/built for
	# i686-nanos with absolute /work paths) so configure regenerates cleanly for x86_64.
	rm -f config.status config.h
	find . -name Makefile -delete 2>/dev/null || true
	find . \( -name '*.o' -o -name '*.a' -o -name '*.lo' \) -delete 2>/dev/null || true
	./configure --host="$HOST_TRIPLE" --build=x86_64-pc-linux-gnu \
		--disable-perl-regexp --disable-nls \
		CC="$CC" AWK=awk --cache-file="$STAGE/nx.cache" 2>&1 | tail -20
	# Append the dllimport shim to the bottom of the generated config.h: every gnulib TU does
	# `#include <config.h>` first, so by the time the shim runs the config.h guard is already
	# set (gnulib's stdio.h wrapper is happy) and stdin/stdout/stderr/environ/_ctype_b get
	# redirected to their libc.ndl IAT slots — the x86_64 RIP-relative data-import requirement.
	printf '\n#include "%s"\n' "$NANOS/user/libc-glue/nx-dllimport.h" >> "$STAGE/config.h"
	# getprogname/setprogname are NanOS libc functions but the (unpatched) /opt picolibc headers
	# don't declare them; gnulib's error() calls getprogname() -> implicit-decl error on gcc14.
	printf 'const char *getprogname(void);\nvoid setprogname(const char *);\n' >> "$STAGE/config.h"
	# Build only the library + the grep binary; skip gnulib-tests (its networking probes pull in
	# headers NanOS lacks, and the test programs are never shipped).
	make -C lib 2>&1 | tail -10
	make -C src 2>&1 | tail -15
	BIN="$STAGE/src/grep"
	;;
vim)
	seed_cache
	# vim's autoconf runs a few target programs; supply the answers it cannot cross-run.
	cat >> "$STAGE/nx.cache" <<-'EOF'
	vim_cv_toupper_broken=no
	vim_cv_terminfo=yes
	vim_cv_tgent=zero
	vim_cv_getcwd_broken=no
	vim_cv_stat_ignores_slash=no
	vim_cv_memmove_handles_overlap=yes
	vim_cv_bcopy_handles_overlap=yes
	vim_cv_memcpy_handles_overlap=no
	vim_cv_tty_group=world
	ac_cv_sizeof_time_t=8
	EOF
	# vim's autotools state lives under src/auto/; scrub the stale i686 cache + generated files.
	rm -f src/auto/config.cache src/auto/config.status src/auto/config.h
	find . -name Makefile -delete 2>/dev/null || true
	find . \( -name '*.o' -o -name '*.a' \) -delete 2>/dev/null || true
	( cd src && ./configure --host="$HOST_TRIPLE" --build=x86_64-pc-linux-gnu \
		--with-features=normal --disable-gui --without-x --enable-gui=no \
		--disable-nls --disable-channel --disable-netbeans --with-tlib=tinfo \
		CC="$CC" AWK=awk --cache-file="$STAGE/nx.cache" ) 2>&1 | tail -30
	make 2>&1 | tail -30
	BIN="$STAGE/src/vim"
	;;
*)
	echo "nx-port-build.sh: unknown app '$APP'" >&2
	exit 2
	;;
esac

echo "== mknx -> $APP.nxe =="
"$MKNX" "$BIN" "$SRCDIR/$APP.nxe" --need libc.ndl
ls -l "$SRCDIR/$APP.nxe"

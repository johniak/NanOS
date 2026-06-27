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
toybox)
	# toybox is a 0BSD multicall binary (login/su/passwd/id/groups/...). Not autotools — its own
	# kconfig + scripts/make.sh. We build a MINIMAL set (the user-identity tools) against the NanOS
	# libc via nx-gcc; HOSTCC=cc builds toybox's own generator helpers natively.
	#
	# Three source adaptations (toybox is Linux-centric; NanOS is Linux-syscall-ish but bare-elf):
	#  1. include <sys/statfs.h> — toybox only does so under __linux__; struct statfs is otherwise
	#     incomplete where lib/portability.h's statfs_bsize() inlines dereference it.
	#  2. enable the Linux code paths for __nanos__ in lib/portability.c (dev_major/minor/makedev
	#     use the Linux dev_t encoding, which NanOS shares; otherwise they #error). Built with
	#     -D__nanos__.
	#  3. strip a trailing ".nxe" from argv[0] before applet dispatch — NanOS runs programs as
	#     "<name>.nxe", so basename(argv[0]) is e.g. "login.nxe"; toy_find needs "login".
	sed -i "/#include <sys\/mount.h>/a #include <sys/statfs.h>" lib/portability.h
	sed -i "s/#if defined(__linux__)/#if defined(__linux__) || defined(__nanos__)/g" lib/portability.c
	sed -i "s@char \*ss = basename(s);@char *ss = basename(s); {char*_d=strstr(ss,\".nxe\"); if(_d\&\&!_d[4])*_d=0;}@" main.c
	# Minimal config: everything off (allnoconfig), then enable the multiplexer + SUID handling +
	# the identity commands. CONFIG_TOYBOX_SUID lets one setuid-root toybox.nxe drop privilege for
	# the non-suid applets (id/groups) while su/passwd keep it.
	make allnoconfig >/dev/null 2>&1
	for s in TOYBOX TOYBOX_SUID TOYBOX_HELP LOGIN SU PASSWD ID GROUPS WHOAMI TRUE FALSE; do
		sed -i "s/^# CONFIG_${s} is not set/CONFIG_${s}=y/" .config
		grep -q "^CONFIG_${s}=y" .config || echo "CONFIG_${s}=y" >> .config
	done
	yes "" | make oldconfig >/dev/null 2>&1 || true
	make CROSS_COMPILE= CC="$CC" HOSTCC=cc CFLAGS=-D__nanos__ 2>&1 | tail -20
	BIN="$STAGE/toybox"
	;;
sudo)
	# Real sudo (Todd Miller, ISC license) with the sudoers policy linked STATICALLY into the
	# front-end (--disable-shared --enable-static-sudoers: NanOS has no dlopen for .so plugins).
	# Auth is passwd/shadow via crypt (no PAM). Seed the cross answers configure cannot probe by
	# running a target binary, plus the NanOS libc facts (killpg exists; use the SYSTEM getopt so
	# sudo does not compile its bundled getopt_long.c, whose <compat/getopt.h> would clash with
	# the getopt-import shim). CPPFLAGS: -D__nanos__ (Linux code paths) + the getopt DATA-import
	# shim (optarg/optind/... arrive via libc.ndl __imp_ slots).
	cat > "$STAGE/nx.cache" <<-'EOF'
	ac_cv_sizeof_long=8
	ac_cv_sizeof_void_p=8
	ac_cv_sizeof_size_t=8
	ac_cv_sizeof_off_t=8
	ac_cv_sizeof_time_t=8
	ac_cv_c_bigendian=no
	ac_cv_func_killpg=yes
	ac_cv_func_getopt_long=yes
	EOF
	./configure --host="$HOST_TRIPLE" --build=x86_64-pc-linux-gnu \
		--disable-shared --enable-static-sudoers --disable-nls --without-pam --without-ldap \
		--without-sssd --disable-pie --disable-hardening --disable-openssl \
		CC="$CC" AWK=awk "CPPFLAGS=-D__nanos__ -include nx-getopt-import.h -include string.h" \
		--cache-file="$STAGE/nx.cache" 2>&1 | tail -15
	make 2>&1 | tail -20
	BIN="$STAGE/src/sudo"
	;;
sqlite3)
	# SQLite ships as a single amalgamation (sqlite3.c) + the CLI front-end (shell.c) — there is
	# NO configure step, so we compile directly with the cross gcc against picolibc + the NanOS
	# libc-glue headers, then link the real `sqlite3` command-line shell as a NanOS .nxe program
	# (crt0/nxhdr + the .nxe linker script + the libc import library). compat.c (in the fork)
	# supplies the advisory-lock no-op fcntl; everything else (fsync/ftruncate/...) is the real
	# RW-ext4 path. The mounted fork tree holds sqlite3.c/sqlite3.h/shell.c at its root and
	# compat.c under nanos/ (this STAGE is a copy of it).
	CFLAGS_SQ="-ffreestanding -isystem $PICO/include -D_DEFAULT_SOURCE -D_GNU_SOURCE \
		-fno-pic -fno-stack-protector -fcommon -Os -w $NX_ARCHFLAGS \
		-I$NANOS/user/libc-glue/include -iquote $NANOS/kernel \
		-include $NANOS/user/libc-glue/compat-decls.h \
		-include $NANOS/user/libc-glue/nx-dllimport.h"   # errno/stdin/stdout/stderr -> libc.ndl IAT slots
	# Port config: the unix VFS, single-threaded, no extension loading (no dlopen), no WAL (needs
	# shared-memory -shm mmap), no popen. DIRSYNC off (NanOS doesn't fsync a directory fd); TEMP
	# tables in memory. fdatasync/fsync/ftruncate are REAL now, so the DB on /disks/main is durable.
	SQLITE_FLAGS="-DSQLITE_OS_UNIX=1 -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION \
		-DSQLITE_MAX_MMAP_SIZE=0 -DSQLITE_DEFAULT_MMAP_SIZE=0 -DSQLITE_OMIT_WAL \
		-DSQLITE_DISABLE_DIRSYNC=1 -DSQLITE_TEMP_STORE=3 -DSQLITE_OMIT_POPEN \
		-DHAVE_FDATASYNC=1 -DHAVE_USLEEP=1 -DHAVE_READLINK=0 -DHAVE_LSTAT=1 \
		-DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_ENABLE_LOCKING_STYLE=0"

	echo "== compile sqlite3.c (amalgamation) =="
	$NX_CC $CFLAGS_SQ $SQLITE_FLAGS -c "$STAGE/sqlite3.c" -o "$STAGE/sqlite3.o"
	echo "== compile shell.c (CLI front-end) =="
	$NX_CC $CFLAGS_SQ $SQLITE_FLAGS -I"$STAGE" -c "$STAGE/shell.c" -o "$STAGE/shell.o"
	echo "== compile compat.c (advisory-lock fcntl no-op) =="
	$NX_CC $CFLAGS_SQ -c "$STAGE/nanos/compat.c" -o "$STAGE/compat.o"

	echo "== link the sqlite3 CLI (.nxe program: crt0 + nxhdr + libc import lib) =="
	$NX_CC -nostdlib -Wl,--emit-relocs -T "$NX_LDSCRIPT" -o "$STAGE/sqlite3" \
		"$NANOS/bin/crt0.o" "$NANOS/bin/nxhdr.o" \
		"$STAGE/shell.o" "$STAGE/sqlite3.o" "$STAGE/compat.o" "$NANOS/bin/libc.ndl.a" -lgcc
	BIN="$STAGE/sqlite3"

	# Also carry the SQLite shared library forward (the original port's deliverable): libsqlite.ndl
	# exports the public sqlite3_* C API so OTHER NanOS apps can import it by name (Windows-style),
	# plus an import library to link against. Non-fatal: a failure here never blocks the CLI .nxe.
	echo "== build libsqlite.ndl shared library (+ import lib) =="
	if ( set -e
		cd "$STAGE"
		$NX_CC -nostdlib -Wl,--emit-relocs -T "$NANOS/user/dll.ld" -o libsqlite.elf \
			"$NANOS/bin/nxhdr.o" sqlite3.o compat.o "$NANOS/bin/libc.ndl.a" -lgcc
		"$MKNX" libsqlite.elf "$SRCDIR/libsqlite.ndl" --dll --export-all --need libc.ndl
		rm -rf imp && mkdir -p imp
		"$MKNX" libsqlite.elf imp --implib --export-all --soname libsqlite.ndl
		for f in imp/*.s; do nasm -f elf64 "$f" -o "${f%.s}.o"; done
		rm -f "$SRCDIR/libsqlite.ndl.a"; ar rcs "$SRCDIR/libsqlite.ndl.a" imp/*.o
		ls -l "$SRCDIR/libsqlite.ndl" "$SRCDIR/libsqlite.ndl.a"
	); then echo "== libsqlite.ndl built =="; else echo "WARN: libsqlite.ndl build skipped/failed (CLI still produced)"; fi
	;;
*)
	echo "nx-port-build.sh: unknown app '$APP'" >&2
	exit 2
	;;
esac

echo "== mknx -> $APP.nxe =="
"$MKNX" "$BIN" "$SRCDIR/$APP.nxe" --need libc.ndl
ls -l "$SRCDIR/$APP.nxe"

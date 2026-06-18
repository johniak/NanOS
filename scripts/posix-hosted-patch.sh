#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# posix-hosted-patch.sh — adapt a copied picolibc i686-elf header tree so the i686-nanos target
# sees a hosted POSIX surface instead of the bare-metal one. picolibc gates a lot of standard
# POSIX behind __CYGWIN__/__rtems__/__SPU__ (target identity) rather than __POSIX_VISIBLE
# (feature level), so a stock cross gcc — even with _GNU_SOURCE — never sees those declarations.
#
# We make __nanos__ a hosted citizen for exactly the pieces NanOS actually implements, WITHOUT
# inheriting rtems/cygwin struct layouts (which would break the libc.ndl ABI). Every edit is
# guarded so re-running stays idempotent. NanOS provides: lstat/mknod, nanosleep/clock_gettime,
# and sigaction (whose sa_flags are accepted-but-ignored, so the flag bits are pure no-ops);
# getrlimit/setrlimit are declared so configure-probing apps compile — the kernel stub reports
# "no limit", which is the honest answer for a single-flat-address-space OS.
#
# Usage: posix-hosted-patch.sh <sysroot-include-dir>
set -e
INC="$1"
[ -n "$INC" ] && [ -d "$INC" ] || { echo "usage: posix-hosted-patch.sh <include-dir>"; exit 1; }

# --- sys/stat.h: lstat/mknod + UTIME_NOW/UTIME_OMIT (function/macro guards, not struct layout) ---
STAT="$INC/sys/stat.h"
if [ -f "$STAT" ] && ! grep -q '__nanos__' "$STAT"; then
	sed -i.bak \
		-e 's@^#if defined (__SPU__) || defined(__rtems__) || defined(__CYGWIN__)$@#if defined (__SPU__) || defined(__rtems__) || defined(__CYGWIN__) || defined(__nanos__)@' \
		-e 's@^#if defined(__CYGWIN__) || defined(__rtems__)$@#if defined(__CYGWIN__) || defined(__rtems__) || defined(__nanos__)@' \
		"$STAT" && rm -f "$STAT.bak"
	echo "  patched sys/stat.h (lstat/mknod, UTIME_NOW/OMIT)"
fi

# --- sys/features.h: take cygwin's POSIX capability block (_POSIX_TIMERS etc.). It is pure
#     capability macros to EOF (no cygwin-only types), so nanosleep/clock_gettime become visible. ---
FEAT="$INC/sys/features.h"
if [ -f "$FEAT" ] && ! grep -q '__CYGWIN__) || defined(__nanos__)' "$FEAT"; then
	sed -i.bak 's@^#ifdef __CYGWIN__$@#if defined(__CYGWIN__) || defined(__nanos__)@' "$FEAT" && rm -f "$FEAT.bak"
	echo "  patched sys/features.h (_POSIX_TIMERS -> nanosleep/clock_gettime)"
fi

# --- sys/signal.h: the minimal (non-rtems/non-cygwin) sigaction lacks the sa_flags bit names.
#     NanOS ignores sa_flags entirely, so define the standard bits as harmless no-ops. We do NOT
#     define SA_SIGINFO (it would enable 3-arg handler code paths that need the rtems struct). ---
SIG="$INC/sys/signal.h"
if [ -f "$SIG" ] && ! grep -q 'NanOS sa_flags' "$SIG"; then
	awk '
	  { print }
	  /^#define SA_NOCLDSTOP 1  \/\* only value supported now for sa_flags \*\// {
	    print "#ifdef __nanos__"
	    print "/* NanOS sa_flags are accepted but not separately honored (kernel signals carry fixed"
	    print "   BSD-restart semantics), so these standard flag bits are defined as no-ops. */"
	    print "#define SA_ONSTACK   0x00000001"
	    print "#define SA_RESETHAND 0x00000002"
	    print "#define SA_NODEFER   0x00000004"
	    print "#define SA_RESTART   0x00000008"
	    print "#define SA_NOCLDWAIT 0x00000010"
	    print "#define SA_NOCLDSTOP_BIT 0x00000020"
	    print "#endif /* __nanos__ */"
	  }
	' "$SIG" > "$SIG.tmp" && mv "$SIG.tmp" "$SIG"
	echo "  patched sys/signal.h (SA_ONSTACK/SA_RESTART... no-op flag bits)"
fi

# --- stdlib.h: getprogname/setprogname (BSD). picolibc omits them; NanOS provides them
#     (libc-glue, seeded from argv[0] by crt0). gnulib's error()/coreutils want getprogname. ---
STDLIB="$INC/stdlib.h"
if [ -f "$STDLIB" ] && ! grep -q 'getprogname' "$STDLIB"; then
	{ echo "#ifdef __nanos__"; echo "const char* getprogname(void);"; \
	  echo "void setprogname(const char*);"; echo "#endif"; } >> "$STDLIB"
	echo "  patched stdlib.h (getprogname/setprogname)"
fi

# --- stdio.h: <sys/features.h> advertises _POSIX_THREAD_SAFE_FUNCTIONS for __nanos__ (it was
#     taken from the cygwin block for nanosleep/clock_gettime), which promises flockfile and the
#     getc_unlocked() family. picolibc's stdio.h never declares them, so that promise is a lie —
#     gnulib (getopt, closeout, ...) then calls undeclared flockfile. NanOS stdio is single-
#     threaded per process, so FILE locking is a genuine no-op and the *_unlocked variants equal
#     the locked ones; declaring them here makes the advertised capability truthful. ---
STDIO="$INC/stdio.h"
if [ -f "$STDIO" ] && ! grep -q 'NanOS stdio is single-threaded' "$STDIO"; then
	awk '
	  /^#endif \/\* _STDIO_H_ \*\/$/ && !done {
	    print "#ifdef __nanos__"
	    print "/* NanOS stdio is single-threaded per process: FILE locking is a no-op and the"
	    print "   *_unlocked variants are identical to the locked ones, so _POSIX_THREAD_SAFE_FUNCTIONS"
	    print "   (advertised in <sys/features.h>) is honoured truthfully. */"
	    print "static __inline void flockfile(FILE *__f) { (void)__f; }"
	    print "static __inline void funlockfile(FILE *__f) { (void)__f; }"
	    print "static __inline int  ftrylockfile(FILE *__f) { (void)__f; return 0; }"
	    print "#ifndef getc_unlocked"
	    print "#define getc_unlocked(fp)    getc(fp)"
	    print "#define putc_unlocked(c, fp) putc((c), (fp))"
	    print "#define getchar_unlocked()   getchar()"
	    print "#define putchar_unlocked(c)  putchar(c)"
	    print "#endif"
	    print "#endif /* __nanos__ */"
	    done = 1
	  }
	  { print }
	' "$STDIO" > "$STDIO.tmp" && mv "$STDIO.tmp" "$STDIO"
	echo "  patched stdio.h (flockfile/funlockfile/ftrylockfile + getc_unlocked family no-ops)"
fi

# --- stdio_ext.h: glibc's __fpending (used by gnulib closeout). picolibc has no such header;
#     ship a minimal one. NanOS implements __fpending (libc-glue) as a safe 0. ---
EXT="$INC/stdio_ext.h"
if [ ! -e "$EXT" ]; then
	{ echo "#ifndef _STDIO_EXT_H"; echo "#define _STDIO_EXT_H"; echo "#include <stdio.h>"; \
	  echo "#ifdef __cplusplus"; echo 'extern "C" {'; echo "#endif"; \
	  echo "size_t __fpending(FILE*);"; \
	  echo "#ifdef __cplusplus"; echo "}"; echo "#endif"; echo "#endif"; } > "$EXT"
	echo "  added stdio_ext.h (__fpending)"
fi

# --- dirent.h: the BSD/glibc d_type constants (DT_DIR, DT_REG, ...). picolibc's minimal dirent.h
#     omits them; many tools (grep -r, find, ...) reference them. Standard values. ---
DIRENT="$INC/dirent.h"
if [ -f "$DIRENT" ] && ! grep -q 'DT_DIR' "$DIRENT"; then
	{ echo "#ifdef __nanos__"; echo "#define DT_UNKNOWN 0"; echo "#define DT_FIFO 1";
	  echo "#define DT_CHR 2"; echo "#define DT_DIR 4"; echo "#define DT_BLK 6";
	  echo "#define DT_REG 8"; echo "#define DT_LNK 10"; echo "#define DT_SOCK 12";
	  echo "#define DT_WHT 14"; echo "#endif"; } >> "$DIRENT"
	echo "  patched dirent.h (DT_* constants)"
fi

# --- sys/utime.h: picolibc declares struct utimbuf but leaves utime() to a per-arch override
#     that i686-elf does not ship. NanOS exports utime, so add the prototype. ---
UT="$INC/sys/utime.h"
if [ -f "$UT" ] && ! grep -q 'int utime' "$UT"; then
	awk '
	  /^#endif \/\* _SYS_UTIME_H \*\/$/ {
	    print "#ifdef __nanos__"
	    print "int utime(const char*, const struct utimbuf*);"
	    print "#endif /* __nanos__ */"
	  }
	  { print }
	' "$UT" > "$UT.tmp" && mv "$UT.tmp" "$UT"
	echo "  patched sys/utime.h (utime prototype)"
fi

# --- sys/resource.h: picolibc ships only getrusage/struct rusage. Add the rlimit surface that
#     POSIX apps probe; the NanOS stub returns RLIM_INFINITY (no limit). ---
RES="$INC/sys/resource.h"
if [ -f "$RES" ] && ! grep -q 'getrlimit' "$RES"; then
	awk '
	  /^#endif \/\* !_SYS_RESOURCE_H_ \*\/$/ {
	    print "#ifdef __nanos__"
	    print "struct rlimit { unsigned long rlim_cur; unsigned long rlim_max; };"
	    print "#define RLIM_INFINITY (~0UL)"
	    print "#define RLIMIT_CPU    0"
	    print "#define RLIMIT_FSIZE  1"
	    print "#define RLIMIT_DATA   2"
	    print "#define RLIMIT_STACK  3"
	    print "#define RLIMIT_CORE   4"
	    print "#define RLIMIT_NOFILE 5"
	    print "#define RLIMIT_AS     6"
	    print "#define RLIMIT_NPROC  7"
	    print "#define RLIMIT_MEMLOCK 8"
	    print "#define RLIM_NLIMITS  9"
	    print "/* No rlim_t typedef: autoconf apps (e.g. vim) #define their own when missing,"
	    print "   and our struct uses unsigned long directly, so we avoid clashing with that. */"
	    print "int getrlimit(int, struct rlimit*);"
	    print "int setrlimit(int, const struct rlimit*);"
	    print "#endif /* __nanos__ */"
	  }
	  { print }
	' "$RES" > "$RES.tmp" && mv "$RES.tmp" "$RES"
	echo "  patched sys/resource.h (getrlimit/setrlimit + RLIMIT_*)"
fi

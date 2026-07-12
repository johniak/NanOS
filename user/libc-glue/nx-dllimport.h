/*
 * nx-dllimport.h — make picolibc's stdio/errno DATA symbols importable from libc.ndl.
 *
 * Force-included (-include) ONLY when compiling dynamically-linked PROGRAM objects (never
 * the glue that lives inside libc.ndl). picolibc exposes stdin/stdout/stderr as data
 * objects and errno as a data variable; a program references them by address, which a
 * shared library can't satisfy without dllimport-style indirection. So we redefine each
 * to a dereference of an IAT slot (`__imp_<name>`, provided by dllimport.S) that the loader
 * fills with the symbol's address inside libc.ndl — the Windows `__declspec(dllimport)`
 * model. `putchar`/`getchar` expand to fputc/fgetc on stdout/stdin, so they route through
 * here too.
 *
 * Order matters: pull picolibc's own declarations FIRST, then #undef + #define — otherwise
 * the macro would corrupt `extern FILE *const stdout;`.
 */
#ifndef NX_DLLIMPORT_H
#define NX_DLLIMPORT_H

/* Ports splice this into CFLAGS (-include), and some builds (busybox) feed CFLAGS to .S
 * files too — C declarations are "no such instruction" to the assembler. No-op there. */
#ifndef __ASSEMBLER__

/* picolibc's ctype classification table `_ctype_b` is a const DATA export of libc.ndl, and
 * the ctype macros (isalpha/isdigit/...) index it by address. Route it through its dllimport
 * slot like the stream/errno data below. This MUST come before any <ctype.h> is pulled in:
 * with the macro active, the header's own `extern const char _ctype_b[];` expands to
 * `extern const char (*__imp__ctype_b)[];` — the very slot declaration we want (a pointer the
 * loader fills with the table's address). We also declare it here so a TU that reaches
 * `_ctype_b` without including <ctype.h> still resolves (a compatible redeclaration). */
extern const char (*__imp__ctype_b)[];
#define _ctype_b (*__imp__ctype_b)

#include <stdio.h>
#include <errno.h>

#undef stdin
#undef stdout
#undef stderr

/* Each slot holds the ADDRESS of the corresponding object in libc.ndl; dereferencing it
 * yields the value the program expects (a FILE* for the streams). */
extern FILE **__imp_stdin;
extern FILE **__imp_stdout;
extern FILE **__imp_stderr;
extern char ***__imp_environ;

#define stdin   (*__imp_stdin)
#define stdout  (*__imp_stdout)
#define stderr  (*__imp_stderr)
#define environ (*__imp_environ)

/* h_errno is the resolver's DATA export (libc-glue resolv.c). Same RIP-relative problem as
 * the streams; same slot treatment. A later `extern int h_errno;` in <netdb.h> expands to a
 * compatible redeclaration of the slot. */
extern int *__imp_h_errno;
#define h_errno (*__imp_h_errno)

/* errno is NOT a data slot: picolibc is built with -Derrno-function=__errno_location, so
 * <errno.h> already expands `errno` to `(*__errno_location())`. __errno_location is an
 * ordinary FUNCTION export of libc.ndl (glue: tls.c) the loader resolves like any code
 * symbol, returning &__pthread_self()->__errno — a per-thread cell. Nothing to redirect
 * here; we deliberately do NOT #undef/redefine errno (the old `__imp_errno` slot is gone). */

#endif /* !__ASSEMBLER__ */

#endif

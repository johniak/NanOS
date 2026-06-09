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

#include <stdio.h>
#include <errno.h>

#undef stdin
#undef stdout
#undef stderr
#undef errno

/* Each slot holds the ADDRESS of the corresponding object in libc.ndl; dereferencing it
 * yields the value the program expects (a FILE* for the streams, an int lvalue for errno). */
extern FILE **__imp_stdin;
extern FILE **__imp_stdout;
extern FILE **__imp_stderr;
extern int   *__imp_errno;
extern char ***__imp_environ;

#define stdin   (*__imp_stdin)
#define stdout  (*__imp_stdout)
#define stderr  (*__imp_stderr)
#define errno   (*__imp_errno)
#define environ (*__imp_environ)

#endif

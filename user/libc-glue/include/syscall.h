/*
 * syscall.h — NanOS exposes syscalls to userland as named libc functions (libc.ndl imports),
 * not a Linux numeric multiplexer. Provide syscall() for ports that call it directly; the
 * libc-glue implementation returns -ENOSYS for every number EXCEPT SYS_kcmp (see posixstubs.c).
 * Only the SYS_* numbers a port actually references are defined here — deliberately NOT
 * SYS_ioprio_get/set, so htop's ioprio code (guarded by #ifdef SYS_ioprio_get) stays compiled out.
 */
#pragma once

long syscall(long number, ...);

/* kcmp(2), KCMP_FILE only — the one number syscall() actually forwards to the kernel. Mesa's
 * os_same_file_description() uses it to prove two DRM fds share a GEM handle namespace (iris
 * would otherwise fall back to a dma-buf PRIME roundtrip NanOS doesn't support). Must match
 * kernel/SyscallNr.h (real Linux x86_64 number). */
#ifndef SYS_kcmp
#define SYS_kcmp 312
#endif

/* htop reads a process's permitted capabilities via this raw syscall; NanOS has no capabilities,
 * so syscall(SYS_capget,...) returns -ENOSYS and htop treats the process as unprivileged. The
 * value is the Linux x86_64 number, for recognisability only — the result is -ENOSYS regardless. */
#ifndef SYS_capget
#define SYS_capget 125
#endif

/* toybox lib/portability.c calls syscall(SYS_renameat2,...) on the Linux path. NanOS's syscall()
 * returns -ENOSYS, so this just needs to be defined for the source to compile (the Linux x86_64
 * number, for recognisability). renameat2 is not reached by the commands we ship (login/su/...). */
#ifndef SYS_renameat2
#define SYS_renameat2 316
#endif

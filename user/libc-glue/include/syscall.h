/*
 * syscall.h — NanOS's kernel uses the Linux x86_64 syscall numbers, and libc-glue's syscall() is a
 * real generic multiplexer (posixstubs.c) that forwards `number` + args straight to the kernel
 * (unimplemented numbers come back -ENOSYS). So we expose the kernel's authoritative number list
 * (SyscallNr.h, staged into this sysroot) — this is what lets V8/abseil's raw-syscall paths
 * (syscall(SYS_write), syscall(SYS_mmap), sched_*, getcpu) compile and run without per-site patches.
 */
#pragma once

long syscall(long number, ...);

/* The kernel's Linux-x86_64 SYS_* numbers (its __x86_64__ section applies here). Staged into the
 * sysroot include by the port's sysroot-refresh step. */
#include <SyscallNr.h>

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

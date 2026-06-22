/*
 * syscall.h — NanOS exposes syscalls to userland as named libc functions (libc.ndl imports),
 * not a Linux numeric multiplexer. Provide syscall() for ports that call it directly; the
 * libc-glue implementation returns -ENOSYS for every number (see posixstubs.c). Only the SYS_*
 * numbers a port actually references are defined here — deliberately NOT SYS_ioprio_get/set, so
 * htop's ioprio code (guarded by #ifdef SYS_ioprio_get) stays compiled out.
 */
#pragma once

long syscall(long number, ...);

/* htop reads a process's permitted capabilities via this raw syscall; NanOS has no capabilities,
 * so syscall(SYS_capget,...) returns -ENOSYS and htop treats the process as unprivileged. The
 * value is the Linux x86_64 number, for recognisability only — the result is -ENOSYS regardless. */
#ifndef SYS_capget
#define SYS_capget 125
#endif

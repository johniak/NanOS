/*
 * SyscallNr.h — syscall numbers (Linux i386 ABI).
 *
 * Plain C, no namespace, so the userland runtime (libnanos.c, compiled as C) can
 * share the exact numbers with the kernel dispatch. This is our stable syscall
 * ABI contract between user programs and the kernel.
 */
#ifndef SYSCALLNR_H_
#define SYSCALLNR_H_

#define SYS_exit 1
#define SYS_fork 2
#define SYS_read 3
#define SYS_write 4
#define SYS_open 5
#define SYS_close 6
#define SYS_waitpid 7
#define SYS_execve 11
#define SYS_lseek 19
#define SYS_kill 37
#define SYS_signal 48
#define SYS_ioctl 54
#define SYS_mmap2 192
#define SYS_stat 106
#define SYS_fstat 108
#define SYS_sigreturn 119
#define SYS_sigprocmask 126
#define SYS_getdents64 220

/* NanOS-private numbers (outside the Linux i386 range, so they never collide with
 * a Linux number we might add later). */
#define SYS_termmode 501  /* console input mode: 0 = cooked (line), 1 = raw (keys) */

#endif /* SYSCALLNR_H_ */

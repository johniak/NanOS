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
#define SYS_getpid 20
#define SYS_getppid 64
#define SYS_read 3
#define SYS_write 4
#define SYS_open 5
#define SYS_close 6
#define SYS_waitpid 7
#define SYS_unlink 10
#define SYS_mkdir 39
#define SYS_chdir 12
#define SYS_getcwd 183
#define SYS_execve 11
#define SYS_lseek 19
#define SYS_times 43
#define SYS_brk 45
#define SYS_kill 37
#define SYS_signal 48
#define SYS_nanosleep 162
#define SYS_clock_gettime 265
#define SYS_ioctl 54
#define SYS_fcntl 55
#define SYS_mmap2 192
#define SYS_stat 106
#define SYS_lstat 107
#define SYS_readlink 85
#define SYS_fstat 108
#define SYS_sigreturn 119
#define SYS_sigprocmask 126
#define SYS_getdents64 220
#define SYS_dup 41
#define SYS_pipe 42
#define SYS_dup2 63
#define SYS_setpgid 57
#define SYS_getpgrp 65
#define SYS_setsid 66
#define SYS_getpgid 132
#define SYS_getsid 147
#define SYS_poll 168

/* NanOS-private numbers (outside the Linux i386 range, so they never collide with
 * a Linux number we might add later). */
#define SYS_termmode 501  /* console input mode: 0 = cooked (line), 1 = raw (keys) */

#endif /* SYSCALLNR_H_ */

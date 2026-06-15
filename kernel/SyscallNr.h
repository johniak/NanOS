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
#define SYS_clone 120        /* thread/process creation (pthread keystone) */
#define SYS_exit_group 252   /* terminate the whole thread group */
#define SYS_getpid 20
#define SYS_getppid 64
#define SYS_read 3
#define SYS_write 4
#define SYS_open 5
#define SYS_close 6
#define SYS_waitpid 7
#define SYS_unlink 10
#define SYS_mkdir 39
#define SYS_rmdir 40
#define SYS_rename 38
#define SYS_link 9
#define SYS_symlink 83
#define SYS_truncate 92
#define SYS_ftruncate 93
#define SYS_chmod 15
#define SYS_fchmod 94
#define SYS_chown 182
#define SYS_lchown 16
#define SYS_fchown 95
#define SYS_utime 30
#define SYS_utimes 271
#define SYS_utimensat 320
#define SYS_access 33
#define SYS_faccessat 307
#define SYS_statfs 99
#define SYS_fstatfs 100
#define SYS_fsync 118
#define SYS_fdatasync 148
#define SYS_sync 36
#define SYS_umask 60
#define SYS_fchdir 133
#define SYS_creat 8
#define SYS_openat 295
#define SYS_mkdirat 296
#define SYS_unlinkat 301
#define SYS_renameat 302
#define SYS_renameat2 353
#define SYS_linkat 303
#define SYS_symlinkat 304
#define SYS_readlinkat 305
#define SYS_fchmodat 306
#define SYS_fchownat 298
#define SYS_fstatat64 300
#define SYS_getuid 24
#define SYS_getuid32 199
#define SYS_geteuid 49
#define SYS_geteuid32 201
#define SYS_getgid 47
#define SYS_getgid32 200
#define SYS_getegid 50
#define SYS_getegid32 202
#define SYS_setuid 23
#define SYS_setuid32 213
#define SYS_setgid 46
#define SYS_setgid32 214
#define SYS_chdir 12
#define SYS_getcwd 183
#define SYS_execve 11
#define SYS_lseek 19
#define SYS_times 43
#define SYS_brk 45
#define SYS_kill 37
#define SYS_signal 48
#define SYS_pause 29
#define SYS_sigsuspend 72   /* NanOS passes the wait-mask in the first arg (our sigset_t is one
                             * 32-bit word; the legacy i386 history slots are unused) */
#define SYS_nanosleep 162
#define SYS_futex 240
#define SYS_gettid 224
#define SYS_set_thread_area 243
#define SYS_set_tid_address 258
#define SYS_clock_gettime 265
#define SYS_getrandom 355
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
#define SYS_reboot 88      /* power the machine off (arch::powerOff); does not return */
#define SYS__newselect 142 /* select(2) on i386 (the "new" 5-arg form) */
/* Sockets: the legacy socketcall(2) multiplexer + the modern direct calls (Linux >=4.3 i386).
 * send/recv exist ONLY as socketcall sub-calls (9/10), never as direct i386 syscalls. */
#define SYS_socketcall 102
#define SYS_socket 359
#define SYS_socketpair 360
#define SYS_bind 361
#define SYS_connect 362
#define SYS_listen 363
#define SYS_accept4 364
#define SYS_getsockopt 365
#define SYS_setsockopt 366
#define SYS_getsockname 367
#define SYS_getpeername 368
#define SYS_sendto 369
#define SYS_sendmsg 370
#define SYS_recvfrom 371
#define SYS_recvmsg 372
#define SYS_shutdown 373
/* socketcall sub-call numbers (the index in the (call, args*) pair). */
#define SC_SOCKET 1
#define SC_BIND 2
#define SC_CONNECT 3
#define SC_LISTEN 4
#define SC_ACCEPT 5
#define SC_GETSOCKNAME 6
#define SC_GETPEERNAME 7
#define SC_SOCKETPAIR 8
#define SC_SEND 9
#define SC_RECV 10
#define SC_SENDTO 11
#define SC_RECVFROM 12
#define SC_SHUTDOWN 13
#define SC_SETSOCKOPT 14
#define SC_GETSOCKOPT 15
#define SC_SENDMSG 16
#define SC_RECVMSG 17
#define SC_ACCEPT4 18

/* NanOS-private numbers (outside the Linux i386 range, so they never collide with
 * a Linux number we might add later). */
#define SYS_termmode 501  /* console input mode: 0 = cooked (line), 1 = raw (keys) */

#endif /* SYSCALLNR_H_ */

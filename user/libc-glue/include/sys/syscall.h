/* sys/syscall.h — toybox's toys.h includes this for the raw syscall() multiplexer. NanOS
 * exposes syscalls as named libc functions, so syscall() returns -ENOSYS (posixstubs.c). The
 * SYS_* numbers a port references are defined in <syscall.h>. */
#ifndef _SYS_SYSCALL_H
#define _SYS_SYSCALL_H
#include <syscall.h>
#endif
#define SYS_gettid 224

/*
 * linux/futex.h — the FUTEX_* operation constants for ports that call SYS_futex directly (abseil's
 * SpinLock, V8). NanOS's kernel implements a Linux-ABI futex (SYS_futex 202) used by the pthread
 * port, so these raw calls work through the generic syscall(); this header just supplies the op
 * numbers the source needs at compile time. Values are the Linux uapi values.
 */
#ifndef _LINUX_FUTEX_H
#define _LINUX_FUTEX_H

#define FUTEX_WAIT              0
#define FUTEX_WAKE              1
#define FUTEX_FD                2
#define FUTEX_REQUEUE           3
#define FUTEX_CMP_REQUEUE       4
#define FUTEX_WAKE_OP           5
#define FUTEX_LOCK_PI           6
#define FUTEX_UNLOCK_PI         7
#define FUTEX_TRYLOCK_PI        8
#define FUTEX_WAIT_BITSET       9
#define FUTEX_WAKE_BITSET       10

#define FUTEX_PRIVATE_FLAG      128
#define FUTEX_CLOCK_REALTIME    256
#define FUTEX_CMD_MASK          ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME)

#define FUTEX_WAIT_PRIVATE      (FUTEX_WAIT | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_PRIVATE      (FUTEX_WAKE | FUTEX_PRIVATE_FLAG)

#define FUTEX_BITSET_MATCH_ANY  0xffffffff

#endif /* _LINUX_FUTEX_H */

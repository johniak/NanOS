/* linux/capability.h — minimal Linux capability ABI shim for ports (htop) that read a process's
 * permitted capabilities via the raw capget syscall. NanOS has no capability model, so
 * syscall(SYS_capget,...) returns -ENOSYS and the returned data is never consumed — these types
 * exist only so the (always-compiled) call site builds. */
#ifndef _NANOS_LINUX_CAPABILITY_H
#define _NANOS_LINUX_CAPABILITY_H
#include <stdint.h>

#define _LINUX_CAPABILITY_VERSION_3 0x20080522

struct __user_cap_header_struct { uint32_t version; int pid; };
struct __user_cap_data_struct { uint32_t effective, permitted, inheritable; };

#endif

/* linux/capability.h — minimal Linux capability ABI shim for ports (htop) that read a process's
 * permitted capabilities via the raw capget syscall. NanOS has no capability model, so
 * syscall(SYS_capget,...) returns -ENOSYS and the returned data is never consumed — these types
 * exist only so the (always-compiled) call site builds. */
#ifndef _NANOS_LINUX_CAPABILITY_H
#define _NANOS_LINUX_CAPABILITY_H
#include <stdint.h>

#define _LINUX_CAPABILITY_VERSION_3 0x20080522
#define _LINUX_CAPABILITY_U32S_3    2

struct __user_cap_header_struct { uint32_t version; int pid; };
struct __user_cap_data_struct { uint32_t effective, permitted, inheritable; };

/* Node's node_credentials.cc probes CAP_NET_BIND_SERVICE. capget() returns -ENOSYS on NanOS, so the
 * probe reports "capability not held" and Node treats the process (which is root) accordingly. */
#define CAP_NET_BIND_SERVICE 10
#define CAP_LAST_CAP         40
#define CAP_TO_INDEX(x) ((x) >> 5)          /* which 32-bit word */
#define CAP_TO_MASK(x)  (1 << ((x) & 31))   /* which bit within it */
#define cap_valid(x)    ((x) >= 0 && (x) <= CAP_LAST_CAP)

#endif

/*
 * sys/prctl.h — minimal prctl(2) for ports that name anonymous memory regions (abseil's
 * low_level_alloc uses PR_SET_VMA/PR_SET_VMA_ANON_NAME to label V8 arenas for debugging). NanOS's
 * kernel does not implement prctl, so prctl() forwards through the generic syscall() and comes back
 * -ENOSYS; every caller here treats the result as advisory and ignores it. The constants exist so the
 * source compiles; the value is the Linux number, for recognisability.
 */
#ifndef _SYS_PRCTL_H
#define _SYS_PRCTL_H

#ifdef __cplusplus
extern "C" {
#endif

#define PR_SET_VMA            0x53564d41
#define PR_SET_VMA_ANON_NAME  0

int prctl(int option, ...);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_PRCTL_H */

/* bb_compat.h — force-included into busybox (CONFIG_EXTRA_CFLAGS) to declare the few GNU/POSIX
 * functions picolibc omits but libc.ndl provides. Keeps busybox sources unpatched. */
#ifndef _NANOS_BB_COMPAT_H
#define _NANOS_BB_COMPAT_H
#ifndef __ASSEMBLER__
#ifdef __cplusplus
extern "C" {
#endif
int clearenv(void);
#ifdef __cplusplus
}
#endif
#endif
#endif

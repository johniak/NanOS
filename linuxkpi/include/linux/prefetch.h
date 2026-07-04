/* linuxkpi/include/linux/prefetch.h — cache prefetch hints. */
#ifndef _LINUXKPI_LINUX_PREFETCH_H
#define _LINUXKPI_LINUX_PREFETCH_H
static inline void prefetch(const void *x){ __builtin_prefetch(x, 0); }
static inline void prefetchw(const void *x){ __builtin_prefetch(x, 1); }
#endif

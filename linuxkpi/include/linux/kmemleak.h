/*
 * linuxkpi/include/linux/kmemleak.h — no-op kmemleak shim.
 *
 * NanOS has no kmemleak detector; these annotations are pure debug hints in Linux.
 */
#ifndef _LKPI_KMEMLEAK_H
#define _LKPI_KMEMLEAK_H
#include <linux/types.h>
static inline void kmemleak_alloc(const void *p, size_t s, int c, gfp_t g){ (void)p;(void)s;(void)c;(void)g; }
static inline void kmemleak_free(const void *p){ (void)p; }
static inline void kmemleak_not_leak(const void *p){ (void)p; }
static inline void kmemleak_ignore(const void *p){ (void)p; }
static inline void kmemleak_no_scan(const void *p){ (void)p; }
static inline void kmemleak_update_trace(const void *p){ (void)p; }
#endif /* _LKPI_KMEMLEAK_H */

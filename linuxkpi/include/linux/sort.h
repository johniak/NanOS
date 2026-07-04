/*
 * linuxkpi/include/linux/sort.h — sort()/bsearch() for the LinuxKPI shim (kpi_sort.c).
 * Linux signature: heapsort with an optional swap callback.
 */
#ifndef _LINUXKPI_LINUX_SORT_H
#define _LINUXKPI_LINUX_SORT_H

#include <linux/types.h>

#ifdef __cplusplus
extern "C" {
#endif

void sort(void *base, size_t num, size_t size,
          int (*cmp)(const void *, const void *),
          void (*swap)(void *, void *, int size));

void *bsearch(const void *key, const void *base, size_t num, size_t size,
              int (*cmp)(const void *, const void *));

/* sort_r: like sort() but the comparator (and optional swap) receive a caller `priv` pointer.
 * i915 sorts small arrays (e.g. VBT child devices, engine lists); implemented in kpi_sort.c. */
void sort_r(void *base, size_t num, size_t size,
            int (*cmp)(const void *, const void *, const void *priv),
            void (*swap)(void *, void *, int size),
            const void *priv);

/* collision-free engine entry (bsearch aliases this in the kext; host tests call it). */
void *lkpi_bsearch(const void *key, const void *base, size_t num, size_t size,
                   int (*cmp)(const void *, const void *));

#ifdef __cplusplus
}
#endif

#endif /* _LINUXKPI_LINUX_SORT_H */

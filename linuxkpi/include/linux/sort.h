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

#ifdef __cplusplus
}
#endif

#endif /* _LINUXKPI_LINUX_SORT_H */

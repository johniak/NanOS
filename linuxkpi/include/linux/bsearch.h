/* linuxkpi/include/linux/bsearch.h — bsearch() (implemented in kpi_sort.c, declared in sort.h). */
#ifndef _LINUXKPI_LINUX_BSEARCH_H
#define _LINUXKPI_LINUX_BSEARCH_H
#include <linux/types.h>
void *bsearch(const void *key, const void *base, size_t num, size_t size,
	      int (*cmp)(const void *, const void *));
#endif

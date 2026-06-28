/*
 * linuxkpi/include/linux/slab.h — kmalloc family for the NanOS LinuxKPI shim, backed by
 * the kernel heap (knx_malloc/knx_free). Every block carries a small size header so
 * krealloc/ksize work without a slab; payload is returned 16-byte aligned.
 */
#ifndef _LINUXKPI_LINUX_SLAB_H
#define _LINUXKPI_LINUX_SLAB_H

#include <linux/types.h>
#include <linux/gfp.h>
#ifndef NANOS_HOST_TEST
#include <linux/mm.h>   /* slab pulls mm in mainline; gives page helpers to .c that only include slab.h */
#endif

#ifdef __cplusplus
extern "C" {
#endif

void *kmalloc(size_t size, gfp_t flags);
void *kzalloc(size_t size, gfp_t flags);
void *kcalloc(size_t n, size_t size, gfp_t flags);
void *kmalloc_array(size_t n, size_t size, gfp_t flags);
void *krealloc(const void *p, size_t new_size, gfp_t flags);
void  kfree(const void *p);
size_t ksize(const void *p);

/* Linux aliases that map onto the same heap for the shim. */
void *kvmalloc(size_t size, gfp_t flags);
void *kvzalloc(size_t size, gfp_t flags);
void  kvfree(const void *p);
void *vmalloc(size_t size);
void *vzalloc(size_t size);
void  vfree(const void *p);

char *kstrdup(const char *s, gfp_t flags);

#ifdef __cplusplus
}
#endif

#endif /* _LINUXKPI_LINUX_SLAB_H */

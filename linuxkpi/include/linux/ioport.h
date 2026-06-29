#ifndef _LKPI_IOPORT_H
#define _LKPI_IOPORT_H
#include <linux/types.h>
struct resource { resource_size_t start, end; const char *name; unsigned long flags; struct resource *parent, *sibling, *child; };
#define IORESOURCE_MEM 0x00000200
#define IORESOURCE_IO  0x00000100
#define IORESOURCE_BUSY 0x80000000
extern struct resource iomem_resource;
static inline resource_size_t resource_size(const struct resource *r){ return r->end - r->start + 1; }
#define DEFINE_RES_MEM(_start,_size) (struct resource){ .start=(_start), .end=(_start)+(_size)-1, .flags=IORESOURCE_MEM }
#endif

#ifndef _LKPI_IOPORT_H
#define _LKPI_IOPORT_H
#include <linux/types.h>
struct resource { resource_size_t start, end; const char *name; unsigned long flags; unsigned long desc; struct resource *parent, *sibling, *child; };
#define IORESOURCE_MEM 0x00000200
#define IORESOURCE_IO  0x00000100
#define IORESOURCE_BUSY 0x80000000
extern struct resource iomem_resource;
static inline resource_size_t resource_size(const struct resource *r){ return r->end - r->start + 1; }
#define DEFINE_RES_MEM(_start,_size) (struct resource){ .start=(_start), .end=(_start)+(_size)-1, .flags=IORESOURCE_MEM }
/* does r1 fully contain r2? */
static inline bool resource_contains(const struct resource *r1, const struct resource *r2){ if(!r1||!r2) return false; return r1->start <= r2->start && r1->end >= r2->end; }
static inline int release_resource(struct resource *r){ (void)r; return 0; }
static inline struct resource *request_mem_region(unsigned long start, unsigned long n, const char *name){ (void)start;(void)n;(void)name; return 0; }
static inline void release_mem_region(unsigned long start, unsigned long n){ (void)start;(void)n; }
#ifndef IORESOURCE_UNSET
#define IORESOURCE_UNSET   0x20000000
#define IORESOURCE_BUSY    0x80000000
#define IORESOURCE_SIZEALIGN 0x00020000
#define IORESOURCE_PREFETCH 0x00002000
#define IORESOURCE_MEM_64   0x00100000
#define IORESOURCE_WINDOW   0x00200000
#define IORESOURCE_IRQ      0x00000400
#define IORESOURCE_DMA      0x00000800
#define IORESOURCE_IRQ_HIGHEDGE 0x00000001
#endif
#ifndef IORES_DESC_NONE
#define IORES_DESC_NONE                 0
#define IORES_DESC_RESERVED             8
#define IORES_DESC_DEVICE_PRIVATE_MEMORY 6
#endif
#ifndef DEFINE_RES_IRQ
#define DEFINE_RES_IRQ(_irq) (struct resource){ .start=(_irq), .end=(_irq), .flags=IORESOURCE_IRQ }
#define DEFINE_RES_IRQ_NAMED(_irq,_name) (struct resource){ .start=(_irq), .end=(_irq), .flags=IORESOURCE_IRQ }
#endif
#endif

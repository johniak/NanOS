/*
 * linuxkpi/include/linux/io.h — MMIO + ioremap for the shim, backed by knx_map_mmio.
 * Identity-mapped kernel: virt==phys. MMIO accessors are volatile loads/stores.
 */
#ifndef _LINUXKPI_LINUX_IO_H
#define _LINUXKPI_LINUX_IO_H

#include <linux/types.h>
#include <lkpi_knx.h>

static inline void *ioremap(phys_addr_t phys, unsigned long size) { return knx_map_mmio((unsigned)phys, (unsigned)size); }
static inline void *ioremap_wc(phys_addr_t phys, unsigned long size) { return knx_map_mmio((unsigned)phys, (unsigned)size); }
static inline void *ioremap_cache(phys_addr_t phys, unsigned long size) { return knx_map_mmio((unsigned)phys, (unsigned)size); }
static inline void  iounmap(volatile void *addr) { (void)addr; }
static inline phys_addr_t virt_to_phys_io(const volatile void *a) { return (phys_addr_t)(unsigned long)a; }

static inline u8  readb(const volatile void *a) { return *(const volatile u8 *)a; }
static inline u16 readw(const volatile void *a) { return *(const volatile u16 *)a; }
static inline u32 readl(const volatile void *a) { return *(const volatile u32 *)a; }
static inline u64 readq(const volatile void *a) { return *(const volatile u64 *)a; }
static inline void writeb(u8 v, volatile void *a)  { *(volatile u8 *)a = v; }
static inline void writew(u16 v, volatile void *a) { *(volatile u16 *)a = v; }
static inline void writel(u32 v, volatile void *a) { *(volatile u32 *)a = v; }
static inline void writeq(u64 v, volatile void *a) { *(volatile u64 *)a = v; }

/* little-endian relaxed variants (x86 is LE) */
#define ioread8(a)        readb(a)
#define ioread16(a)       readw(a)
#define ioread32(a)       readl(a)
#define iowrite8(v, a)    writeb(v, a)
#define iowrite16(v, a)   writew(v, a)
#define iowrite32(v, a)   writel(v, a)
#define readb_relaxed(a)  readb(a)
#define readw_relaxed(a)  readw(a)
#define readl_relaxed(a)  readl(a)
#define readq_relaxed(a)  readq(a)
#define writeb_relaxed(v, a) writeb(v, a)
#define writew_relaxed(v, a) writew(v, a)
#define writel_relaxed(v, a) writel(v, a)
#define writeq_relaxed(v, a) writeq(v, a)

#endif /* _LINUXKPI_LINUX_IO_H */

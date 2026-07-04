/*
 * linuxkpi/include/linux/io.h — MMIO + ioremap for the shim, backed by knx_map_mmio.
 * Identity-mapped kernel: virt==phys. MMIO accessors are volatile loads/stores.
 */
#ifndef _LINUXKPI_LINUX_IO_H
#define _LINUXKPI_LINUX_IO_H

#include <linux/types.h>
#include <lkpi_knx.h>

/* PAT (page attribute table) is not exposed to the shim; write-combining is handled by ioremap_wc
 * directly, so i915 can treat PAT as available (its WC-mapping path is a no-op cost here). */
static inline bool pat_enabled(void){ return true; }
/* wrap an errno as an __iomem ERR_PTR (i915 ioremap error paths). */
#define IOMEM_ERR_PTR(err) ((void __iomem *)(long)(err))
/* MTRR write-combine add/del: NanOS maps device BARs WC via ioremap_wc already, so these succeed
 * with a positive handle and free is a no-op. */
static inline int arch_phys_wc_add(unsigned long base, unsigned long size){ (void)base;(void)size; return 0; }
static inline void arch_phys_wc_del(int handle){ (void)handle; }
static inline unsigned long arch_phys_wc_index(int handle){ (void)handle; return 0; }
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

#ifndef _LKPI_IO_EXTRA
#define _LKPI_IO_EXTRA
#include <linux/ioport.h>
static inline void *devm_ioremap(struct device *d, phys_addr_t o, unsigned long s){ (void)d; return knx_map_mmio((unsigned)o,(unsigned)s); }
static inline void *devm_ioremap_wc(struct device *d, phys_addr_t o, unsigned long s){ (void)d; return knx_map_mmio((unsigned)o,(unsigned)s); }
#endif

#ifndef _LKPI_IO_MEMCPY
#define _LKPI_IO_MEMCPY
#include <linux/string.h>
static inline void memcpy_toio(volatile void *d, const void *s, size_t n){ for(size_t i=0;i<n;i++)((volatile char*)d)[i]=((const char*)s)[i]; }
static inline void memcpy_fromio(void *d, const volatile void *s, size_t n){ for(size_t i=0;i<n;i++)((char*)d)[i]=((const volatile char*)s)[i]; }
static inline void memset_io(volatile void *d, int c, size_t n){ for(size_t i=0;i<n;i++)((volatile char*)d)[i]=c; }
#endif

/* 64-bit MMIO + x86 port I/O + IRQ-flag control. i915 touches legacy VGA ports (outb/inb) and a few
 * local_irq_save/disable sites. The port/flag ops are real x86 instructions in the kext; under the
 * host doctest build they are no-ops (no I/O ports, tests never take these paths). */
#ifndef _LKPI_IO_PORT
#define _LKPI_IO_PORT
#define ioread64(a)    readq(a)
#define iowrite64(v,a) writeq(v,a)
#ifdef NANOS_HOST_TEST
static inline void outb(u8 v, u16 p){ (void)v;(void)p; }
static inline void outw(u16 v, u16 p){ (void)v;(void)p; }
static inline void outl(u32 v, u16 p){ (void)v;(void)p; }
static inline u8  inb(u16 p){ (void)p; return 0; }
static inline u16 inw(u16 p){ (void)p; return 0; }
static inline u32 inl(u16 p){ (void)p; return 0; }
static inline void local_irq_disable(void){}
static inline void local_irq_enable(void){}
#define local_irq_save(f)    do { (f) = 0; } while (0)
#define local_irq_restore(f) do { (void)(f); } while (0)
#else
static inline void outb(u8 v, u16 p){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline void outw(u16 v, u16 p){ __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p)); }
static inline void outl(u32 v, u16 p){ __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p)); }
static inline u8  inb(u16 p){ u8 v;  __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline u16 inw(u16 p){ u16 v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline u32 inl(u16 p){ u32 v; __asm__ volatile("inl %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void local_irq_disable(void){ __asm__ volatile("cli":::"memory"); }
static inline void local_irq_enable(void){ __asm__ volatile("sti":::"memory"); }
#define local_irq_save(f)    do { unsigned long __lf; __asm__ volatile("pushfq; pop %0; cli":"=r"(__lf)::"memory"); (f) = __lf; } while (0)
#define local_irq_restore(f) do { __asm__ volatile("push %0; popfq"::"r"((unsigned long)(f)):"memory","cc"); } while (0)
#endif
#endif

/* linuxkpi/include/linux/io-64-nonatomic-lo-hi.h — 64-bit MMIO as two 32-bit accesses. x86_64
 * has native readq/writeq, so these forward to them (order is irrelevant on real 64-bit MMIO). */
#ifndef _LKPI_IO64_NONATOMIC_LO_HI_H
#define _LKPI_IO64_NONATOMIC_LO_HI_H
#include <linux/io.h>
#ifndef lo_hi_readq
static inline u64 lo_hi_readq(const volatile void __iomem *addr){ return readq(addr); }
static inline void lo_hi_writeq(u64 val, volatile void __iomem *addr){ writeq(val, addr); }
static inline u64 hi_lo_readq(const volatile void __iomem *addr){ return readq(addr); }
static inline void hi_lo_writeq(u64 val, volatile void __iomem *addr){ writeq(val, addr); }
#endif
#endif

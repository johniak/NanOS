/*
 * linuxkpi/include/linux/poison.h — memory-poison byte patterns (values from Linux's poison.h).
 * i915 fills unused ring/batch space with POISON_INUSE so stale reads are obvious.
 */
#ifndef _LKPI_LINUX_POISON_H
#define _LKPI_LINUX_POISON_H

#define POISON_INUSE  0x5a
#define POISON_FREE   0x6b
#define POISON_END    0xa5

#endif /* _LKPI_LINUX_POISON_H */

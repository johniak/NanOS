/*
 * linuxkpi/include/linux/log2.h — log2 / power-of-two helpers shim.
 *
 * Provides the subset the DRM/virtio path uses, layered over the primitives already in
 * <linux/kernel.h> (is_power_of_2, ilog2) and <linux/bitops.h> (fls/fls64/__fls). Shadows
 * the vendored real log2.h (the shim include dir comes first) to avoid a conflicting
 * is_power_of_2 redefinition.
 */
#ifndef _LKPI_LOG2_H
#define _LKPI_LOG2_H
#include <linux/types.h>
#include <linux/bitops.h>
#include <linux/kernel.h>   /* is_power_of_2, ilog2, BITS_PER_LONG */

static inline int fls_long(unsigned long l)
{
	return (BITS_PER_LONG == 32) ? fls((unsigned int)l) : fls64((u64)l);
}

static inline __attribute__((const)) unsigned long __roundup_pow_of_two(unsigned long n)
{
	return 1UL << fls_long(n - 1);
}
static inline __attribute__((const)) unsigned long __rounddown_pow_of_two(unsigned long n)
{
	return 1UL << (fls_long(n) - 1);
}

#define roundup_pow_of_two(n)   \
	((n) == 1 ? 1UL : __roundup_pow_of_two((unsigned long)(n)))
#define rounddown_pow_of_two(n) \
	((n) == 1 ? 1UL : __rounddown_pow_of_two((unsigned long)(n)))

#ifndef order_base_2
#define order_base_2(n) ((n) > 1 ? (int)(ilog2((n) - 1) + 1) : 0)
#endif

#define bits_per(n) ((n) == 0 ? 1 : (int)(ilog2(n) + 1))

#endif /* _LKPI_LOG2_H */

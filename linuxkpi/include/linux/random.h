/* linuxkpi/include/linux/random.h — i915 uses randomness for MOCS/ppgtt address salt, not crypto.
 * A TSC-seeded xorshift is sufficient and honest (documented non-cryptographic). */
#ifndef _LINUXKPI_LINUX_RANDOM_H
#define _LINUXKPI_LINUX_RANDOM_H
#include <linux/types.h>
#include <asm/tsc.h>
static inline u32 get_random_u32(void)
{ static u32 s; if(!s) s=(u32)rdtsc()|1u; s^=s<<13; s^=s>>17; s^=s<<5; return s; }
static inline u64 get_random_u64(void){ return ((u64)get_random_u32()<<32)|get_random_u32(); }
static inline u32 get_random_u32_below(u32 ceil){ return ceil ? get_random_u32() % ceil : 0; }
static inline u32 prandom_u32_max(u32 ep){ return get_random_u32_below(ep); }
static inline int get_random_int(void){ return (int)get_random_u32(); }
static inline void get_random_bytes(void *buf, int n){ u8 *p=buf; while(n-->0)*p++=(u8)get_random_u32(); }
#endif

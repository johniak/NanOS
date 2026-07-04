/* linuxkpi/include/linux/prandom.h — seeded pseudo-random (xorshift). Non-cryptographic. */
#ifndef _LINUXKPI_LINUX_PRANDOM_H
#define _LINUXKPI_LINUX_PRANDOM_H
#include <linux/types.h>
#include <linux/random.h>
struct rnd_state { u32 s1, s2, s3, s4; };
static inline u32 prandom_u32_state(struct rnd_state *s)
{ s->s1 ^= s->s1<<13; s->s1 ^= s->s1>>17; s->s1 ^= s->s1<<5; return s->s1; }
static inline void prandom_seed_state(struct rnd_state *s, u64 seed)
{ s->s1=(u32)seed|1u; s->s2=(u32)(seed>>32)|1u; s->s3=1; s->s4=1; }
static inline u32 prandom_u32_max(u32 ep); /* from random.h */
#endif

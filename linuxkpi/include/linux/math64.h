#ifndef _LKPI_MATH64_H
#define _LKPI_MATH64_H
#include <linux/types.h>
static inline u64 div_u64(u64 d, u32 v){return d/v;}
static inline u64 div64_u64(u64 d, u64 v){return d/v;}
static inline s64 div_s64(s64 d, s32 v){return d/v;}
static inline u64 div_u64_rem(u64 d, u32 v, u32* r){*r=d%v;return d/v;}
static inline u64 mul_u32_u32(u32 a, u32 b){return (u64)a*b;}
#endif

#ifndef _LKPI_MATH64_X
#define _LKPI_MATH64_X
static inline u64 div64_u64_rem(u64 d, u64 v, u64 *rem){ *rem = d % v; return d / v; }
static inline s64 div64_s64(s64 d, s64 v){ return d / v; }   /* drm_fixed.h */
/* (a * mul) / div at full 64x32/32 precision via a 128-bit intermediate (i915 RPS/PLL frequency math). */
static inline u64 mul_u64_u32_div(u64 a, u32 mul, u32 div){ return (u64)(((unsigned __int128)a * mul) / div); }
static inline u64 mul_u64_u32_shr(u64 a, u32 mul, unsigned shift){ return (u64)(((unsigned __int128)a * mul) >> shift); }
#endif

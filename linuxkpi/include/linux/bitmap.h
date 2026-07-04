
#ifndef _LKPI_BITMAP_SET
#define _LKPI_BITMAP_SET
#include <linux/bitops.h>
static inline void bitmap_set(unsigned long *map, unsigned start, unsigned nbits){ for(unsigned i=0;i<nbits;i++) __set_bit(start+i, map); }
static inline void bitmap_clear(unsigned long *map, unsigned start, unsigned nbits){ for(unsigned i=0;i<nbits;i++) __clear_bit(start+i, map); }
#endif

#ifndef _LKPI_BITMAP_OPS
#define _LKPI_BITMAP_OPS
/* Forward-declare the allocator rather than including <linux/slab.h>: slab.h pulls mm.h which pulls
 * bitops.h, and bitops.h routes bitmap.h back here — including slab.h would knot that cycle. */
#ifdef __cplusplus
extern "C" {
#endif
void *kmalloc(size_t size, gfp_t flags);
void kfree(const void *p);
#ifdef __cplusplus
}
#endif
#ifndef LKPI_BITS_TO_LONGS
#define LKPI_BITS_TO_LONGS(nbits) (((nbits) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#endif
static inline void bitmap_zero(unsigned long *dst, unsigned nbits){ unsigned n=LKPI_BITS_TO_LONGS(nbits); for(unsigned i=0;i<n;i++) dst[i]=0UL; }
static inline void bitmap_fill(unsigned long *dst, unsigned nbits){ unsigned n=LKPI_BITS_TO_LONGS(nbits); for(unsigned i=0;i<n;i++) dst[i]=~0UL; if(nbits%BITS_PER_LONG) dst[n-1]=(1UL<<(nbits%BITS_PER_LONG))-1; }
static inline int  bitmap_empty(const unsigned long *src, unsigned nbits){ for(unsigned i=0;i<nbits;i++) if(test_bit(i,src)) return 0; return 1; }
static inline unsigned bitmap_weight(const unsigned long *src, unsigned nbits){ unsigned w=0; for(unsigned i=0;i<nbits;i++) if(test_bit(i,src)) w++; return w; }
static inline void bitmap_or(unsigned long *dst, const unsigned long *a, const unsigned long *b, unsigned nbits){ unsigned n=LKPI_BITS_TO_LONGS(nbits); for(unsigned i=0;i<n;i++) dst[i]=a[i]|b[i]; }
static inline unsigned long *bitmap_zalloc(unsigned nbits, gfp_t gfp){ unsigned n=LKPI_BITS_TO_LONGS(nbits); unsigned long *m=(unsigned long*)kmalloc(n*sizeof(unsigned long), gfp); if(m) for(unsigned i=0;i<n;i++) m[i]=0UL; return m; }
static inline void bitmap_free(const unsigned long *m){ kfree((void*)m); }
static inline void bitmap_copy(unsigned long *dst, const unsigned long *src, unsigned nbits){ unsigned n=LKPI_BITS_TO_LONGS(nbits); for(unsigned i=0;i<n;i++) dst[i]=src[i]; }
static inline int  bitmap_andnot(unsigned long *dst, const unsigned long *a, const unsigned long *b, unsigned nbits){ unsigned n=LKPI_BITS_TO_LONGS(nbits); unsigned long r=0; for(unsigned i=0;i<n;i++){ dst[i]=a[i]&~b[i]; r|=dst[i]; } return r!=0; }
static inline int  bitmap_and(unsigned long *dst, const unsigned long *a, const unsigned long *b, unsigned nbits){ unsigned n=LKPI_BITS_TO_LONGS(nbits); unsigned long r=0; for(unsigned i=0;i<n;i++){ dst[i]=a[i]&b[i]; r|=dst[i]; } return r!=0; }
/* Pack a u32 array into an unsigned-long bitmap (LP64: two u32 per long). */
static inline void bitmap_from_arr32(unsigned long *bitmap, const u32 *buf, unsigned nbits){ unsigned nw32=(nbits+31)/32; unsigned nl=LKPI_BITS_TO_LONGS(nbits); for(unsigned i=0;i<nl;i++){ unsigned long lo=buf[2*i]; unsigned long hi=(2*i+1<nw32)?(unsigned long)buf[2*i+1]:0UL; bitmap[i]=lo|(hi<<32); } }
/* a ⊆ b over nbits? (every set bit of a is set in b) */
static inline int bitmap_subset(const unsigned long *a, const unsigned long *b, unsigned nbits){ unsigned n=LKPI_BITS_TO_LONGS(nbits); for(unsigned i=0;i<n;i++) if(a[i] & ~b[i]) return 0; return 1; }
/* a ∩ b non-empty over nbits? */
static inline int bitmap_intersects(const unsigned long *a, const unsigned long *b, unsigned nbits){ unsigned n=LKPI_BITS_TO_LONGS(nbits); for(unsigned i=0;i<n;i++) if(a[i] & b[i]) return 1; return 0; }
static inline int bitmap_equal(const unsigned long *a, const unsigned long *b, unsigned nbits){ unsigned n=LKPI_BITS_TO_LONGS(nbits); for(unsigned i=0;i<n;i++) if(a[i]!=b[i]) return 0; return 1; }
/* allocate a naturally-aligned run of 2^order bits: first-fit scan, set the run, return its start (or -errno). */
static inline int bitmap_find_free_region(unsigned long *bitmap, unsigned bits, int order){ unsigned len=1u<<order; for(unsigned pos=0; pos+len<=bits; pos+=len){ unsigned k; for(k=0;k<len;k++) if(test_bit(pos+k,bitmap)) break; if(k==len){ for(k=0;k<len;k++) __set_bit(pos+k,bitmap); return (int)pos; } } return -12; }
static inline void bitmap_release_region(unsigned long *bitmap, unsigned pos, int order){ unsigned len=1u<<order; for(unsigned k=0;k<len;k++) __clear_bit(pos+k,bitmap); }
/* shift the whole bitmap right by `shift` bits (bit-serial; nbits is small in i915 use). */
static inline void bitmap_shift_right(unsigned long *dst, const unsigned long *src, unsigned shift, unsigned nbits){ for(unsigned i=0;i<nbits;i++){ unsigned s=i+shift; int v = (s<nbits) ? (int)((src[s/(8*sizeof(long))]>>(s%(8*sizeof(long))))&1UL) : 0; if(v) dst[i/(8*sizeof(long))] |= 1UL<<(i%(8*sizeof(long))); else dst[i/(8*sizeof(long))] &= ~(1UL<<(i%(8*sizeof(long)))); } }
static inline void bitmap_shift_left(unsigned long *dst, const unsigned long *src, unsigned shift, unsigned nbits){ for(unsigned i=nbits;i-- >0;){ int s=(int)i-(int)shift; int v = (s>=0) ? (int)((src[s/(8*(int)sizeof(long))]>>(s%(8*(int)sizeof(long))))&1UL) : 0; if(v) dst[i/(8*sizeof(long))] |= 1UL<<(i%(8*sizeof(long))); else dst[i/(8*sizeof(long))] &= ~(1UL<<(i%(8*sizeof(long)))); } }
#endif

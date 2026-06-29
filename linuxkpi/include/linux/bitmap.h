
#ifndef _LKPI_BITMAP_SET
#define _LKPI_BITMAP_SET
#include <linux/bitops.h>
static inline void bitmap_set(unsigned long *map, unsigned start, unsigned nbits){ for(unsigned i=0;i<nbits;i++) __set_bit(start+i, map); }
static inline void bitmap_clear(unsigned long *map, unsigned start, unsigned nbits){ for(unsigned i=0;i<nbits;i++) __clear_bit(start+i, map); }
#endif

#ifndef _LKPI_CEC_H
#define _LKPI_CEC_H
#define CEC_PHYS_ADDR_INVALID 0xffff
static inline void cec_phys_addr_invalidate(void *a){ (void)a; }
static inline unsigned short cec_get_edid_phys_addr(const void *edid, unsigned size, unsigned *off){ (void)edid;(void)size; if(off)*off=0; return CEC_PHYS_ADDR_INVALID; }
#endif

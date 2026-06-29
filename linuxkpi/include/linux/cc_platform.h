#ifndef _LKPI_CC_PLATFORM_H
#define _LKPI_CC_PLATFORM_H
enum cc_attr { CC_ATTR_MEM_ENCRYPT=0, CC_ATTR_HOST_MEM_ENCRYPT, CC_ATTR_GUEST_MEM_ENCRYPT };
static inline int cc_platform_has(enum cc_attr a){ (void)a; return 0; }
#endif

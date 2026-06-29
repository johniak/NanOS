#ifndef _LKPI_STRING_HELPERS_H
#define _LKPI_STRING_HELPERS_H
#include <linux/types.h>
enum string_size_units { STRING_UNITS_10=0, STRING_UNITS_2 };
static inline int string_get_size(u64 size, u64 blk, enum string_size_units u, char *buf, int len){ (void)size;(void)blk;(void)u; if(len>0)buf[0]=0; return 0; }
#endif

/* linuxkpi/include/asm/byteorder.h — x86_64 little-endian. The __le/__be conversions live in the
 * shim's <linux/types.h>; this header exists so <asm/byteorder.h> includers resolve. */
#ifndef _LKPI_ASM_BYTEORDER_H
#define _LKPI_ASM_BYTEORDER_H
#include <linux/types.h>
#ifndef __LITTLE_ENDIAN
#define __LITTLE_ENDIAN 1234
#endif
#endif

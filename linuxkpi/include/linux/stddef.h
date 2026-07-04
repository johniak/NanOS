/* linuxkpi/include/linux/stddef.h — NULL/offsetof/sizeof_field. */
#ifndef _LKPI_LINUX_STDDEF_H
#define _LKPI_LINUX_STDDEF_H
#include <linux/types.h>
#ifndef NULL
#define NULL ((void*)0)
#endif
#ifndef offsetof
#define offsetof(t,m) __builtin_offsetof(t,m)
#endif
#ifndef sizeof_field
#define sizeof_field(t,m) (sizeof(((t*)0)->m))
#endif
#endif

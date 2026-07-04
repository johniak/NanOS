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
/* A flexible array inside a union/struct, as Linux's <linux/stddef.h>: the empty guard member makes
 * the flex array valid where C would otherwise reject a bare `TYPE name[]` (e.g. i915_syncmap's
 * union of seqno[]/child[]). */
#ifndef DECLARE_FLEX_ARRAY
#define DECLARE_FLEX_ARRAY(TYPE, NAME) \
	struct { struct { } __empty_ ## NAME; TYPE NAME[]; }
#endif
#endif

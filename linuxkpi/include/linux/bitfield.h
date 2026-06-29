#ifndef _LKPI_BITFIELD_H
#define _LKPI_BITFIELD_H
#include <linux/bitops.h>
#define __bf_shf(x) (__builtin_ffsll(x) - 1)
#define FIELD_GET(_mask, _reg) ((typeof(_mask))(((_reg) & (_mask)) >> __bf_shf(_mask)))
#define FIELD_PREP(_mask, _val) (((typeof(_mask))(_val) << __bf_shf(_mask)) & (_mask))
#define FIELD_FIT(_mask, _val) (!((((typeof(_mask))(_val)) << __bf_shf(_mask)) & ~(_mask)))
#endif

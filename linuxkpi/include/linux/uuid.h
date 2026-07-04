#ifndef _LKPI_UUID_H
#define _LKPI_UUID_H
#include <linux/types.h>
#include <linux/string.h>
typedef struct { unsigned char b[16]; } uuid_le;
/* Length of a formatted UUID string "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (no NUL), as Linux. i915
 * perf sizes its OA-config uuid[] buffer with it. */
#ifndef UUID_STRING_LEN
#define UUID_STRING_LEN 36
#endif
static inline void uuid_copy(uuid_t *d, const uuid_t *s){ memcpy(d,s,sizeof(*d)); }
static inline void import_uuid(uuid_t *d, const unsigned char *s){ memcpy(d->b,s,16); }
static inline void export_uuid(unsigned char *d, const uuid_t *s){ memcpy(d,s->b,16); }
static inline bool uuid_equal(const uuid_t *a, const uuid_t *b){ return memcmp(a,b,sizeof(*a))==0; }
#endif

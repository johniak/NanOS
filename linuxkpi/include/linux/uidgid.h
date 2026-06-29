#ifndef _LKPI_UIDGID_H
#define _LKPI_UIDGID_H
typedef struct { unsigned val; } kuid_t;
typedef struct { unsigned val; } kgid_t;
#define overflowuid 65534
#define overflowgid 65534
static inline unsigned __kuid_val(kuid_t u){ return u.val; }
static inline unsigned __kgid_val(kgid_t g){ return g.val; }
static inline unsigned from_kuid_munged(void *ns, kuid_t u){ (void)ns; return u.val; }
static inline unsigned from_kgid_munged(void *ns, kgid_t g){ (void)ns; return g.val; }
#endif

#ifndef _LKPI_UACCESS_H
#define _LKPI_UACCESS_H
#include <linux/string.h>
static inline unsigned long copy_to_user(void *to, const void *from, unsigned long n){ memcpy(to,from,n); return 0; }
static inline unsigned long copy_from_user(void *to, const void *from, unsigned long n){ memcpy(to,from,n); return 0; }
#define get_user(x,p)  ({ (x)=*(p); 0; })
#define put_user(x,p)  ({ *(p)=(x); 0; })
#define access_ok(a,b) 1
#endif

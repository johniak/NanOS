#ifndef _LKPI_UACCESS_H
#define _LKPI_UACCESS_H
#include <linux/string.h>
static inline unsigned long copy_to_user(void *to, const void *from, unsigned long n){ memcpy(to,from,n); return 0; }
static inline unsigned long copy_from_user(void *to, const void *from, unsigned long n){ memcpy(to,from,n); return 0; }
#define get_user(x,p)  ({ (x)=*(p); 0; })
#define put_user(x,p)  ({ *(p)=(x); 0; })
#define access_ok(a,b) 1
/* __-prefixed (access_ok-skipping) variants are identical here — the flat map is always accessible. */
static inline unsigned long __copy_to_user(void *to, const void *from, unsigned long n){ memcpy(to,from,n); return 0; }
static inline unsigned long __copy_from_user(void *to, const void *from, unsigned long n){ memcpy(to,from,n); return 0; }
static inline unsigned long __copy_from_user_inatomic(void *to, const void *from, unsigned long n){ memcpy(to,from,n); return 0; }
static inline unsigned long __copy_to_user_inatomic(void *to, const void *from, unsigned long n){ memcpy(to,from,n); return 0; }
#define __get_user(x,p)  ({ (x)=*(p); 0; })
#define __put_user(x,p)  ({ *(p)=(x); 0; })
#endif

#ifndef _LKPI_UACCESS_MEMDUP
#define _LKPI_UACCESS_MEMDUP
void *memdup_user(const void *src, unsigned long len);
static inline void *memdup_array_user(const void *src, unsigned long n, unsigned long size){ return memdup_user(src, n*size); }
#endif

#ifndef _LKPI_UACCESS_WRITE
#define _LKPI_UACCESS_WRITE
/* user_write_access_begin/end bracket a run of unsafe_put_user. Flat kernel/user map (identity), so
 * "granting write access" is a no-op that always succeeds; unsafe_put_user is a direct store. */
static inline int user_write_access_begin(void __user *ptr, unsigned long len){ (void)ptr;(void)len; return 1; }
static inline void user_write_access_end(void){ }
static inline int user_read_access_begin(const void __user *ptr, unsigned long len){ (void)ptr;(void)len; return 1; }
static inline void user_read_access_end(void){ }
#define unsafe_put_user(x, ptr, err) do { *(ptr) = (x); } while (0)
#define unsafe_get_user(x, ptr, err) do { (x) = *(ptr); } while (0)
/* parse a bool/number written into a user buffer (debugfs). Copies through a small stack buffer. */
static inline int kstrtobool_from_user(const void __user *s, unsigned long count, bool *res){
	char buf[8]; unsigned long n = count < sizeof(buf)-1 ? count : sizeof(buf)-1;
	if (copy_from_user(buf, s, n)) return -14; buf[n] = 0; return kstrtobool(buf, res);
}
static inline int kstrtoull_from_user(const void __user *s, unsigned long count, unsigned int base, unsigned long long *res){
	char buf[32]; unsigned long n = count < sizeof(buf)-1 ? count : sizeof(buf)-1;
	if (copy_from_user(buf, s, n)) return -14; buf[n] = 0; return kstrtoull(buf, base, res);
}
#endif

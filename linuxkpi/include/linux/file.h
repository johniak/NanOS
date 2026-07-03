#ifndef _LKPI_FILE_H
#define _LKPI_FILE_H
#include <linux/fs.h>
struct fd { struct file *file; unsigned int flags; };
static inline struct file *fget(unsigned int fd){ (void)fd; return 0; }
/* fput is REAL (kpi_misc.c): dropping a GEM object's shmem backing must free its pages.
 * As an inline no-op, GEM backing pages were never freed — the kernel heap exhausted under
 * the GL desktop's staging churn and the allocator aliased live buffers (the cross-window
 * texture corruption under a terminal flood). */
void fput(struct file *f);
static inline void fdput(struct fd f){ (void)f; }
#endif

#ifndef _LKPI_FILE_EXTRA
#define _LKPI_FILE_EXTRA
static inline int get_unused_fd_flags(unsigned flags){ (void)flags; return -1; }
static inline void put_unused_fd(unsigned fd){ (void)fd; }
static inline void fd_install(unsigned fd, struct file *f){ (void)fd;(void)f; }
#endif

#ifndef _LKPI_FILE_FDGET
#define _LKPI_FILE_FDGET
static inline struct file *fd_file(struct fd f){ return f.file; }
static inline struct fd fdget(unsigned fd){ (void)fd; struct fd r={0,0}; return r; }
#endif

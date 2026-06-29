#ifndef _LKPI_FILE_H
#define _LKPI_FILE_H
#include <linux/fs.h>
struct fd { struct file *file; unsigned int flags; };
static inline struct file *fget(unsigned int fd){ (void)fd; return 0; }
static inline void fput(struct file *f){ (void)f; }
static inline void fdput(struct fd f){ (void)f; }
#endif

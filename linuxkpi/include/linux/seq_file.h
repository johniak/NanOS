#ifndef _LKPI_SEQ_FILE_H
#define _LKPI_SEQ_FILE_H
#include <linux/types.h>
struct file;
struct inode;
struct seq_file { void *private; };
int seq_printf(struct seq_file*, const char*, ...);
void seq_puts(struct seq_file*, const char*);
void seq_putc(struct seq_file*, char);
void seq_vprintf(struct seq_file*, const char*, __builtin_va_list);
/* seq_file has no output buffer to overflow in the shim (writes are dropped), so it never overflows. */
static inline bool seq_has_overflowed(struct seq_file *m){ (void)m; return false; }
/* single_open/single_release back a one-shot seq_file (i915 debugfs DEFINE_SHOW_ATTRIBUTE). The shim
 * doesn't run a real file's ->show pipeline, so these just stash the private data / succeed. */
static inline int single_open(struct file *f, int (*show)(struct seq_file*, void*), void *data){ (void)f;(void)show;(void)data; return 0; }
static inline int single_open_size(struct file *f, int (*show)(struct seq_file*, void*), void *data, size_t sz){ (void)f;(void)show;(void)data;(void)sz; return 0; }
static inline int single_release(struct inode *i, struct file *f){ (void)i;(void)f; return 0; }
static inline long seq_lseek(struct file *f, long off, int whence){ (void)f;(void)off;(void)whence; return 0; }
#endif

#ifndef _LKPI_SEQ_WRITE
#define _LKPI_SEQ_WRITE
static inline int seq_write(struct seq_file *m, const void *data, size_t len){ (void)m;(void)data;(void)len; return 0; }
#endif

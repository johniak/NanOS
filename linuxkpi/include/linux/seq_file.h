#ifndef _LKPI_SEQ_FILE_H
#define _LKPI_SEQ_FILE_H
#include <linux/types.h>
struct file;
struct inode;
struct seq_file { void *private; char *buf; unsigned long size; unsigned long count; unsigned long from; loff_t index; };
int seq_printf(struct seq_file*, const char*, ...);
void seq_puts(struct seq_file*, const char*);
void seq_putc(struct seq_file*, char);
void seq_vprintf(struct seq_file*, const char*, __builtin_va_list);
/* seq_read backs a file_operations .read for the one-shot seq_file (DEFINE_SHOW_ATTRIBUTE).
 * The shim has no real read pipeline, so the definition (kpi_misc.c) returns 0 (EOF). */
long seq_read(struct file *f, char __user *buf, unsigned long size, loff_t *ppos);
/* seq_file has no output buffer to overflow in the shim (writes are dropped), so it never overflows. */
static inline bool seq_has_overflowed(struct seq_file *m){ (void)m; return false; }
/* single_open/single_release back a one-shot seq_file (i915 debugfs DEFINE_SHOW_ATTRIBUTE). The shim
 * doesn't run a real file's ->show pipeline, so these just stash the private data / succeed. */
static inline int single_open(struct file *f, int (*show)(struct seq_file*, void*), void *data){ (void)f;(void)show;(void)data; return 0; }
static inline int single_open_size(struct file *f, int (*show)(struct seq_file*, void*), void *data, size_t sz){ (void)f;(void)show;(void)data;(void)sz; return 0; }
static inline int single_release(struct inode *i, struct file *f){ (void)i;(void)f; return 0; }
static inline long seq_lseek(struct file *f, long off, int whence){ (void)f;(void)off;(void)whence; return 0; }
/* i915 debugfs nodes are declared with DEFINE_SHOW_ATTRIBUTE(name), which synthesizes a
 * name_open()/name_fops pair wiring ->show through single_open + seq_read/seq_lseek/single_release.
 * Canonical kernel expansion; THIS_MODULE resolves to the shim's module marker. */
#ifndef DEFINE_SHOW_ATTRIBUTE
#define DEFINE_SHOW_ATTRIBUTE(__name)                                       \
static int __name ## _open(struct inode *inode, struct file *file)          \
{                                                                           \
        return single_open(file, __name ## _show, inode->i_private);        \
}                                                                           \
                                                                            \
static const struct file_operations __name ## _fops = {                     \
        .owner          = THIS_MODULE,                                      \
        .open           = __name ## _open,                                  \
        .read           = (void *)seq_read,                                 \
        .llseek         = (void *)seq_lseek,                                \
        .release        = single_release,                                   \
}
/* like DEFINE_SHOW_ATTRIBUTE but the node is also writable (wires ->write to name_write). */
#define DEFINE_SHOW_STORE_ATTRIBUTE(__name)                                 \
static int __name ## _open(struct inode *inode, struct file *file)          \
{                                                                           \
        return single_open(file, __name ## _show, inode->i_private);        \
}                                                                           \
                                                                            \
static const struct file_operations __name ## _fops = {                     \
        .owner          = THIS_MODULE,                                      \
        .open           = __name ## _open,                                  \
        .read           = (void *)seq_read,                                 \
        .write          = (void *)(__name ## _write),                       \
        .llseek         = (void *)seq_lseek,                                \
        .release        = single_release,                                   \
}
#endif
/* i915 debugfs .c files reach us through the vendored <drm/drm_debugfs.h>, which pulls
 * <linux/seq_file.h> (our shim) but NOT <linux/debugfs.h>. Pull debugfs.h here — inside this guard,
 * after struct seq_file is defined — so DEFINE_SIMPLE_ATTRIBUTE / debugfs_create_file are reachable.
 * Being INSIDE the guard is essential: debugfs.h re-includes seq_file.h, and on that re-entry the
 * guard is already set so this include does not re-run (no infinite include recursion). */
#include <linux/debugfs.h>
#endif /* _LKPI_SEQ_FILE_H */

#ifndef _LKPI_SEQ_WRITE
#define _LKPI_SEQ_WRITE
static inline int seq_write(struct seq_file *m, const void *data, size_t len){ (void)m;(void)data;(void)len; return 0; }
#endif

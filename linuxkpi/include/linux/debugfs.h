/* stub: linux/debugfs.h */
#include <linux/seq_file.h>   /* debugfs producers write into a seq_file; i915's *_debugfs.c reach the
                               * full struct seq_file (seq_printf etc.) only through this header. */

#ifndef _LKPI_DEBUGFS_REGSET
#define _LKPI_DEBUGFS_REGSET
struct debugfs_reg32 { char *name; unsigned long offset; };
struct debugfs_regset32 { const struct debugfs_reg32 *regs; int nregs; void *base; struct device *dev; };
static inline void debugfs_print_regs32(struct seq_file *s, const struct debugfs_reg32 *r, int n, void *b, const char *p){ (void)s;(void)r;(void)n;(void)b;(void)p; }
#endif

#ifndef _LKPI_DEBUGFS_X
#define _LKPI_DEBUGFS_X
static inline struct dentry *debugfs_create_dir(const char *n, struct dentry *p){ (void)n;(void)p; return 0; }
static inline void debugfs_remove(struct dentry *d){ (void)d; }
static inline struct dentry *debugfs_create_file(const char *n, unsigned short m, struct dentry *p, void *d, const void *f){ (void)n;(void)m;(void)p;(void)d;(void)f; return 0; }
static inline struct dentry *debugfs_create_file_unsafe(const char *n, unsigned short m, struct dentry *p, void *d, const void *f){ (void)n;(void)m;(void)p;(void)d;(void)f; return 0; }
static inline void debugfs_create_files(void *p, void *d, const void *f, int n){ (void)p;(void)d;(void)f;(void)n; }
#endif

#ifndef _LKPI_DEBUGFS_ATTR
#define _LKPI_DEBUGFS_ATTR
/* DEFINE_DEBUGFS_ATTRIBUTE / DEFINE_SIMPLE_ATTRIBUTE synthesize a name_open()/name file_operations
 * pair around a get/set pair. The shim runs no real file pipeline, so the open just references the
 * get/set (keeping them "used") and the fops is inert. The __get/__set casts keep the static
 * accessor functions referenced so they don't trip -Wunused-function. */
#define DEFINE_DEBUGFS_ATTRIBUTE(__fops, __get, __set, __fmt)                 \
static int __fops ## _open(struct inode *inode, struct file *file)            \
{ (void)inode; (void)file; (void)(__get); (void)(__set); return 0; }          \
static const struct file_operations __fops = {                               \
	.owner = THIS_MODULE,                                                 \
	.open  = __fops ## _open,                                             \
}
#define DEFINE_DEBUGFS_ATTRIBUTE_SIGNED(a,b,c,d) DEFINE_DEBUGFS_ATTRIBUTE(a,b,c,d)
#define DEFINE_SIMPLE_ATTRIBUTE(a,b,c,d)         DEFINE_DEBUGFS_ATTRIBUTE(a,b,c,d)
static inline void debugfs_create_bool(const char *n, unsigned short m, struct dentry *p, bool *v){ (void)n;(void)m;(void)p;(void)v; }
static inline void debugfs_create_u32(const char *n, unsigned short m, struct dentry *p, unsigned *v){ (void)n;(void)m;(void)p;(void)v; }
static inline void debugfs_create_atomic_t(const char *n, unsigned short m, struct dentry *p, void *v){ (void)n;(void)m;(void)p;(void)v; }
#endif

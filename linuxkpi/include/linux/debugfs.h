/* stub: linux/debugfs.h */

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
#endif

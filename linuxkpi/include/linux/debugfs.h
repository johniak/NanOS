/* stub: linux/debugfs.h */

#ifndef _LKPI_DEBUGFS_REGSET
#define _LKPI_DEBUGFS_REGSET
struct debugfs_reg32 { char *name; unsigned long offset; };
struct debugfs_regset32 { const struct debugfs_reg32 *regs; int nregs; void *base; struct device *dev; };
static inline void debugfs_print_regs32(struct seq_file *s, const struct debugfs_reg32 *r, int n, void *b, const char *p){ (void)s;(void)r;(void)n;(void)b;(void)p; }
#endif

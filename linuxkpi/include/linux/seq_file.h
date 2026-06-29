#ifndef _LKPI_SEQ_FILE_H
#define _LKPI_SEQ_FILE_H
#include <linux/types.h>
struct seq_file { void *private; };
int seq_printf(struct seq_file*, const char*, ...);
void seq_puts(struct seq_file*, const char*);
void seq_putc(struct seq_file*, char);
#endif

#ifndef _LKPI_SEQ_WRITE
#define _LKPI_SEQ_WRITE
static inline int seq_write(struct seq_file *m, const void *data, size_t len){ (void)m;(void)data;(void)len; return 0; }
#endif

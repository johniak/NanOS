#ifndef _LKPI_SEQ_FILE_H
#define _LKPI_SEQ_FILE_H
#include <linux/types.h>
struct seq_file { void *private; };
int seq_printf(struct seq_file*, const char*, ...);
void seq_puts(struct seq_file*, const char*);
void seq_putc(struct seq_file*, char);
#endif

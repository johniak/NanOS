#ifndef _LKPI_SEQ_BUF_H
#define _LKPI_SEQ_BUF_H
#include <linux/types.h>
struct seq_buf { char *buffer; size_t size, len; };
#define DECLARE_SEQ_BUF(name, sz) char name##_data[sz]; struct seq_buf name = { name##_data, sz, 0 }
static inline void seq_buf_printf(struct seq_buf *s, const char *fmt, ...){ (void)s;(void)fmt; }
static inline const char *seq_buf_str(struct seq_buf *s){ return s->buffer; }
static inline int seq_buf_has_overflowed(struct seq_buf *s){ return s->len > s->size; }
#endif

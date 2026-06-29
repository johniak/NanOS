#ifndef _LKPI_INTERRUPT_H
#define _LKPI_INTERRUPT_H
#include <linux/types.h>
#include <linux/irqreturn.h>
typedef irqreturn_t (*irq_handler_t)(int, void*);
#define IRQF_SHARED 0x80
static inline int request_irq(unsigned int irq, irq_handler_t h, unsigned long f, const char *n, void *d){ (void)irq;(void)h;(void)f;(void)n;(void)d; return 0; }
static inline void free_irq(unsigned int irq, void *d){ (void)irq;(void)d; }
static inline int request_threaded_irq(unsigned int irq, irq_handler_t h, irq_handler_t th, unsigned long f, const char *n, void *d){ (void)irq;(void)h;(void)th;(void)f;(void)n;(void)d; return 0; }
struct tasklet_struct { void (*func)(unsigned long); unsigned long data; };
static inline void tasklet_schedule(struct tasklet_struct *t){ if(t->func)t->func(t->data); }
#endif

/* linuxkpi/include/linux/irq.h — irq descriptor helpers. The shim routes device IRQs via
 * request_irq/MSI (interrupt.h); i915 includes this for irqreturn_t/irq_handler_t and a few desc
 * accessors that are inert here (no generic irqchip). */
#ifndef _LINUXKPI_LINUX_IRQ_H
#define _LINUXKPI_LINUX_IRQ_H
#include <linux/interrupt.h>
struct irq_desc;
struct irq_data { unsigned int irq; unsigned long hwirq; void *chip_data; };
/* generic irqchip callbacks (i915 GSC builds a tiny irq_chip). Inert: no generic irq domain here. */
struct irq_chip {
	const char *name;
	void (*irq_mask)(struct irq_data *);
	void (*irq_unmask)(struct irq_data *);
	void (*irq_ack)(struct irq_data *);
	void (*irq_eoi)(struct irq_data *);
	int  (*irq_set_type)(struct irq_data *, unsigned int);
};
static inline void *irq_data_get_irq_chip_data(struct irq_data *d){ return d ? d->chip_data : 0; }
static inline unsigned int irqd_to_hwirq(struct irq_data *d){ return d ? (unsigned int)d->hwirq : 0; }
static inline void disable_irq(unsigned int irq){ (void)irq; }
static inline void enable_irq(unsigned int irq){ (void)irq; }
static inline void disable_irq_nosync(unsigned int irq){ (void)irq; }
/* generic-irqchip plumbing (i915 GSC). No irq domain in the shim, so these are inert. */
static inline void irq_set_chip_and_handler_name(unsigned int irq, const struct irq_chip *chip, void *handle, const char *name){ (void)irq;(void)chip;(void)handle;(void)name; }
static inline int irq_set_chip_data(unsigned int irq, void *data){ (void)irq;(void)data; return 0; }
static inline int irq_set_handler_data(unsigned int irq, void *data){ (void)irq;(void)data; return 0; }
static inline int generic_handle_irq(unsigned int irq){ (void)irq; return 0; }
/* irq descriptor allocation: the shim has no generic irq domain; hand back a dummy positive irq. */
static inline int irq_alloc_desc(int node){ (void)node; return 1; }
static inline int irq_alloc_descs(int irq, unsigned int from, unsigned int cnt, int node){ (void)irq;(void)from;(void)cnt;(void)node; return 1; }
static inline void irq_free_desc(unsigned int irq){ (void)irq; }
static inline void irq_free_descs(unsigned int irq, unsigned int cnt){ (void)irq;(void)cnt; }
#define handle_simple_irq ((void *)0)
#endif

/* linuxkpi/include/linux/irq.h — irq descriptor helpers. The shim routes device IRQs via
 * request_irq/MSI (interrupt.h); i915 includes this for irqreturn_t/irq_handler_t and a few desc
 * accessors that are inert here (no generic irqchip). */
#ifndef _LINUXKPI_LINUX_IRQ_H
#define _LINUXKPI_LINUX_IRQ_H
#include <linux/interrupt.h>
struct irq_data; struct irq_desc;
static inline void disable_irq(unsigned int irq){ (void)irq; }
static inline void enable_irq(unsigned int irq){ (void)irq; }
static inline void disable_irq_nosync(unsigned int irq){ (void)irq; }
#endif

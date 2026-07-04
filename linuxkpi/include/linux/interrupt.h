#ifndef _LKPI_INTERRUPT_H
#define _LKPI_INTERRUPT_H
#include <linux/types.h>
#include <linux/irqreturn.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef irqreturn_t (*irq_handler_t)(int, void*);

#define IRQF_SHARED     0x00000080
#define IRQF_ONESHOT    0x00002000
#define IRQF_NO_AUTOEN  0x00080000

/* Real request_irq / free_irq over the kernel's MSI facility (kpi_irq.c). The `irq` number is one
 * handed out by lkpi_irq_bind_msi() — an index into the shim's irq descriptor table, NOT a GSI.
 * A driver's flow mirrors Linux: bind the device's MSI to get an irq number, then request_irq() it. */
int         request_irq(unsigned int irq, irq_handler_t h, unsigned long flags,
                        const char *name, void *dev);
int         request_threaded_irq(unsigned int irq, irq_handler_t h, irq_handler_t thread_fn,
                                 unsigned long flags, const char *name, void *dev);
const void *free_irq(unsigned int irq, void *dev);

/* Bind a PCI function's MSI/MSI-X (via knx_register_msi) to a fresh shim irq number. Returns the
 * irq number to hand request_irq(), or -1 if the function exposes neither MSI nor MSI-X. */
int  lkpi_irq_bind_msi(unsigned bus, unsigned dev, unsigned func);

/* Deliver `irq` as if its interrupt fired: runs the registered hard handler (and, interim until
 * async workqueues land, its thread_fn inline on IRQ_WAKE_THREAD). Called by the MSI trampoline;
 * also the doctest entry point. */
void lkpi_irq_dispatch(int irq);

struct tasklet_struct { void (*func)(unsigned long); unsigned long data; };
static inline void tasklet_schedule(struct tasklet_struct *t){ if(t->func)t->func(t->data); }
#ifdef __cplusplus
}
#endif
#endif

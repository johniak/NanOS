/* linuxkpi/include/linux/mei_aux.h — MEI auxiliary-bus device for the GSC. Gen9 (Comet Lake) has no
 * GSC/HECI graphics controller, so this is an inert placeholder. */
#ifndef _LINUXKPI_LINUX_MEI_AUX_H
#define _LINUXKPI_LINUX_MEI_AUX_H
#include <linux/types.h>
#include <linux/auxiliary_bus.h>
#include <linux/ioport.h>
struct mei_aux_device { struct auxiliary_device aux_dev; int irq; struct resource bar; struct resource ext_op_mem; bool ext_op_mem_used; bool slow_firmware; };
static inline struct mei_aux_device *auxiliary_dev_to_mei_aux_dev(struct auxiliary_device *auxiliary_dev){ return container_of(auxiliary_dev, struct mei_aux_device, aux_dev); }
#endif

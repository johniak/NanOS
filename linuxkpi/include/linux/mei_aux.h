/* linuxkpi/include/linux/mei_aux.h — MEI auxiliary-bus device for the GSC. Gen9 (Comet Lake) has no
 * GSC/HECI graphics controller, so this is an inert placeholder. */
#ifndef _LINUXKPI_LINUX_MEI_AUX_H
#define _LINUXKPI_LINUX_MEI_AUX_H
#include <linux/types.h>
struct mei_aux_device { int id; bool ext_op_mem_used; };
#endif

/* linuxkpi/include/linux/writeback.h — writeback control for i915's shmem swap-out path.
 * NanOS has no writeback/reclaim, so the control block is just carried through inert helpers. */
#ifndef _LINUXKPI_LINUX_WRITEBACK_H
#define _LINUXKPI_LINUX_WRITEBACK_H
#include <linux/types.h>

enum writeback_sync_modes { WB_SYNC_NONE = 0, WB_SYNC_ALL = 1 };

struct writeback_control {
	long nr_to_write;
	long pages_skipped;
	loff_t range_start;
	loff_t range_end;
	enum writeback_sync_modes sync_mode;
	unsigned for_reclaim:1;
	unsigned for_kupdate:1;
	unsigned range_cyclic:1;
};

struct bdi_writeback;
struct address_space;

#endif /* _LINUXKPI_LINUX_WRITEBACK_H */

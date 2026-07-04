/* linuxkpi/include/linux/mmu_notifier.h — STUB (userptr only; not on the bring-up path). Types so the
 * driver compiles; insert/remove are inert. FOLLOW-ON before enabling userptr on the Dell. */
#ifndef _LINUXKPI_LINUX_MMU_NOTIFIER_H
#define _LINUXKPI_LINUX_MMU_NOTIFIER_H
#include <linux/types.h>
struct mm_struct; struct mmu_interval_notifier;
struct mmu_notifier_range { unsigned long start, end; };
struct mmu_interval_notifier_ops {
	bool (*invalidate)(struct mmu_interval_notifier *mni, const struct mmu_notifier_range *range, unsigned long cur_seq);
};
struct mmu_interval_notifier {
	unsigned long start, last;
	const struct mmu_interval_notifier_ops *ops;
};
static inline int mmu_interval_notifier_insert(struct mmu_interval_notifier *mni, struct mm_struct *mm,
		unsigned long start, unsigned long length, const struct mmu_interval_notifier_ops *ops)
{ (void)mm;(void)length; mni->start=start; mni->ops=ops; return 0; }
static inline void mmu_interval_notifier_remove(struct mmu_interval_notifier *mni){ (void)mni; }
static inline unsigned long mmu_interval_read_begin(struct mmu_interval_notifier *mni){ (void)mni; return 0; }
static inline bool mmu_interval_read_retry(struct mmu_interval_notifier *mni, unsigned long seq){ (void)mni;(void)seq; return false; }
static inline void mmu_interval_set_seq(struct mmu_interval_notifier *mni, unsigned long seq){ (void)mni;(void)seq; }
#endif

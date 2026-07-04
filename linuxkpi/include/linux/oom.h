/* linuxkpi/include/linux/oom.h — OOM notifier hooks. NanOS has no OOM killer; register/unregister
 * are no-ops (the i915 oom-notifier that would free purgeable BOs is never called — see shrinker). */
#ifndef _LINUXKPI_LINUX_OOM_H
#define _LINUXKPI_LINUX_OOM_H
struct notifier_block;
static inline int register_oom_notifier(struct notifier_block *nb){ (void)nb; return 0; }
static inline int unregister_oom_notifier(struct notifier_block *nb){ (void)nb; return 0; }
#endif

/* linuxkpi/include/linux/sched/mm.h — memalloc scope guards. NanOS has no reclaim recursion, so the
 * noreclaim/nofs/noio scopes are no-ops (they only gate the shrinker, which is not wired). */
#ifndef _LINUXKPI_LINUX_SCHED_MM_H
#define _LINUXKPI_LINUX_SCHED_MM_H
#include <linux/types.h>
#include <linux/shrinker.h>   /* ttm_pool reaches struct shrinker + shrinker_alloc + SHRINK_EMPTY via <linux/sched/mm.h> transitively, as in mainline */
static inline unsigned int memalloc_noreclaim_save(void) { return 0; }
static inline void memalloc_noreclaim_restore(unsigned int f) { (void)f; }
static inline unsigned int memalloc_nofs_save(void) { return 0; }
static inline void memalloc_nofs_restore(unsigned int f) { (void)f; }
static inline unsigned int memalloc_noio_save(void) { return 0; }
static inline void memalloc_noio_restore(unsigned int f) { (void)f; }
static inline unsigned int memalloc_pin_save(void) { return 0; }
static inline void memalloc_pin_restore(unsigned int f) { (void)f; }
struct mm_struct;
static inline void mmgrab(struct mm_struct *mm) { (void)mm; }
static inline void mmdrop(struct mm_struct *mm) { (void)mm; }
static inline void mmput(struct mm_struct *mm) { (void)mm; }
static inline struct mm_struct *get_task_mm(void *t) { (void)t; return 0; }
#endif

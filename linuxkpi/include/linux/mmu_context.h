/* linuxkpi/include/linux/mmu_context.h — kthread mm borrowing (userptr). Inert on NanOS. */
#ifndef _LINUXKPI_LINUX_MMU_CONTEXT_H
#define _LINUXKPI_LINUX_MMU_CONTEXT_H
struct mm_struct;
static inline void kthread_use_mm(struct mm_struct *mm){ (void)mm; }
static inline void kthread_unuse_mm(struct mm_struct *mm){ (void)mm; }
#endif

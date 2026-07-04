/* linuxkpi/include/linux/wait_bit.h — bit/var waits. NanOS bring-up is cooperative: wake is implicit
 * (waiters re-test), clear_bit_unlock keeps the release-barrier, wait_on_bit spins with cpu_relax. */
#ifndef _LINUXKPI_LINUX_WAIT_BIT_H
#define _LINUXKPI_LINUX_WAIT_BIT_H
#include <linux/wait.h>
#include <linux/bitops.h>   /* clear_bit_unlock / test_bit live here */
#define clear_and_wake_up_bit(bit, word) clear_bit_unlock((bit), (word))
static inline void wake_up_var(void *var) { (void)var; }
static inline int wait_on_bit(unsigned long *word, int bit, unsigned mode)
{ (void)mode; while (test_bit(bit, word)) cpu_relax(); return 0; }
static inline int wait_on_bit_timeout(unsigned long *word, int bit, unsigned mode, unsigned long to)
{ (void)mode; (void)to; while (test_bit(bit, word)) cpu_relax(); return 0; }
#define ___wait_var_event(var, condition, state, exclusive, ret, cmd) do { while (!(condition)) cpu_relax(); } while (0)
#define wait_var_event(var, condition) do { while (!(condition)) cpu_relax(); } while (0)
#define wait_var_event_interruptible(var, condition) ({ while (!(condition)) cpu_relax(); 0; })
#define wait_var_event_killable(var, condition) ({ while (!(condition)) cpu_relax(); 0; })
#define wait_var_event_timeout(var, condition, timeout) ({ while (!(condition)) cpu_relax(); 1; })
/* Wake waiters on a bit-word (paired with wait_on_bit). Cooperative kernel: the bit is already
 * cleared before this call and waiters re-test on their own schedule, so there is nothing to signal. */
static inline void wake_up_bit(void *word, int bit) { (void)word; (void)bit; }
#endif

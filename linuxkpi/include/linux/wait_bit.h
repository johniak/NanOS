/* linuxkpi/include/linux/wait_bit.h — bit/var waits. NanOS bring-up is cooperative: wake is implicit
 * (waiters re-test), clear_bit_unlock keeps the release-barrier. The waits do NOT block on a real
 * waitqueue — they SPIN. A bare cpu_relax() spin was fine for the display path (its condition is met
 * by an MSI the harvester polls), but during the pre-scheduler i915 GT bring-up it deadlocks: the
 * unwind of a probe error (intel_engines_release -> intel_wakeref_wait_for_idle ->
 * wait_var_event_killable) waits on a wakeref that only drops once a PENDING retire/park work item
 * runs — and nothing pumps the workqueue from inside a bare spin, so the work never runs and the box
 * hangs silently forever. Fix (class-wide): every spin turn PUMPS (lkpi_wait_pump = virtio vq +
 * timers + workqueue drain) so deferred work makes progress, and calls lkpi_spin_probe() so a
 * genuinely-starved wait names its call site after ~2 s instead of hanging invisibly. lkpi_wait_pump
 * is declared in <linux/wait.h>; lkpi_spin_probe in kpi_misc.c (declared here for header-only use). */
#ifndef _LINUXKPI_LINUX_WAIT_BIT_H
#define _LINUXKPI_LINUX_WAIT_BIT_H
#include <linux/wait.h>
#include <linux/bitops.h>   /* clear_bit_unlock / test_bit live here */
void lkpi_spin_probe(void *ra);
/* Spin until cond, running `cmd` each turn, PUMPING deferred work, and reporting a starved site. */
#define ___lkpi_spin(cond_expr, cmd) do { \
		void *__lkpi_ra = __builtin_return_address(0); \
		while (!(cond_expr)) { cmd; lkpi_wait_pump(); lkpi_spin_probe(__lkpi_ra); cpu_relax(); } \
	} while (0)
#define clear_and_wake_up_bit(bit, word) clear_bit_unlock((bit), (word))
static inline void wake_up_var(void *var) { (void)var; }
static inline int wait_on_bit(unsigned long *word, int bit, unsigned mode)
{ (void)mode; ___lkpi_spin(!test_bit(bit, word), ); return 0; }
static inline int wait_on_bit_timeout(unsigned long *word, int bit, unsigned mode, unsigned long to)
{ (void)mode; (void)to; ___lkpi_spin(!test_bit(bit, word), ); return 0; }
/* expression form (used as an if()-condition in i915_active): spin running `cmd` until condition, yield `ret`. */
#define ___wait_var_event(var, condition, state, exclusive, ret, cmd) \
	({ (void)(state); (void)(exclusive); ___lkpi_spin((condition), cmd); (ret); })
/* the (single, shared) waitqueue a var-event waiter parks on. One global queue suffices — the shim's
 * wake_up is a barrier and waiters re-check their own condition, so sharing is correct. */
struct wait_queue_head *__var_waitqueue(void *p);
#define wait_var_event(var, condition) do { ___lkpi_spin((condition), ); } while (0)
#define wait_var_event_interruptible(var, condition) ({ ___lkpi_spin((condition), ); 0; })
#define wait_var_event_killable(var, condition) ({ ___lkpi_spin((condition), ); 0; })
#define wait_var_event_timeout(var, condition, timeout) ({ (void)(timeout); ___lkpi_spin((condition), ); 1; })
/* Wake waiters on a bit-word (paired with wait_on_bit). Cooperative kernel: the bit is already
 * cleared before this call and waiters re-test on their own schedule, so there is nothing to signal. */
static inline void wake_up_bit(void *word, int bit) { (void)word; (void)bit; }
#endif

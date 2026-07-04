/* stub: linux/lockdep.h */
#ifndef LOCKDEP_STILL_OK
#define LOCKDEP_STILL_OK 1   /* i915_utils.h references this in its lockdep-annotation helpers */
#endif

/* struct lockdep_map + lockdep_assert_held/is_held/init_map live in the shim's spinlock.h. Only
 * lock_class_key (i915 intel_wakeref embeds two by value) and the set_class helpers are added here. */
#ifndef _LKPI_LOCK_CLASS_KEY
#define _LKPI_LOCK_CLASS_KEY
struct lock_class_key { int dummy; };
#define lockdep_set_class(lock, key) do { (void)(lock); (void)(key); } while (0)
#define lockdep_set_class_and_name(lock, key, name) do { (void)(lock); (void)(key); } while (0)
#define lockdep_set_subclass(lock, sub) do { (void)(lock); } while (0)
#endif

/* lockdep_assert(cond): a lockdep-only runtime assertion (e.g. i915 intel_context asserts a lock is
 * held). With lockdep off it compiles away, exactly as in Linux. lockdep_assert_held lives in
 * spinlock.h; this is the bare-condition form. */
#ifndef lockdep_assert
#define lockdep_assert(cond) do { } while (0)
#endif

/* Lock-pinning cookies: with lockdep off (our config) this mirrors Linux's empty pin_cookie and the
 * no-op pin/repin/unpin macros. i915 stores one by value in i915_request (rq->cookie) around the
 * timeline->mutex it pins during request construction. The macros take the lock object directly. */
#ifndef _LKPI_PIN_COOKIE
#define _LKPI_PIN_COOKIE
struct pin_cookie { unsigned int val; };
#define lockdep_pin_lock(l)      ({ struct pin_cookie __pc = { 0 }; (void)(l); __pc; })
#define lockdep_repin_lock(l, c) do { (void)(l); (void)(c); } while (0)
#define lockdep_unpin_lock(l, c) do { (void)(l); (void)(c); } while (0)
#endif

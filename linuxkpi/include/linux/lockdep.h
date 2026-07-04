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

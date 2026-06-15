#include "pthread_impl.h"
/* NanOS adaptation: dropped `#include <threads.h>` and the `thrd_current` weak_alias.
 * picolibc's <threads.h> pulls <machine/_threads.h>, which is absent for the i686-elf
 * target, and C11 threads are out of scope for Phase 4. Only the POSIX pthread_self alias
 * is kept; thrd_current can be re-added with the C11 threads layer later. */

static pthread_t __pthread_self_internal()
{
	return __pthread_self();
}

weak_alias(__pthread_self_internal, pthread_self);

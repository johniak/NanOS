/*
 * nanos-musl-compat.h — force-included (-include) ahead of every vendored musl pthread
 * source so the bare musl idioms `hidden` / `weak` / `weak_alias` resolve.
 *
 * In upstream musl these macros come from <features.h>, which every musl source pulls in
 * transitively. NanOS builds the pthread core against picolibc's <features.h> (which lacks
 * them), so we provide them here instead of shadowing the system <features.h>. Definitions
 * are musl's verbatim.
 */
#ifndef NANOS_MUSL_COMPAT_H
#define NANOS_MUSL_COMPAT_H

#ifndef weak_alias
#define weak_alias(old, new) \
	extern __typeof(old) new __attribute__((__weak__, __alias__(#old)))
#endif

#ifndef hidden
#define hidden __attribute__((__visibility__("hidden")))
#endif

#ifndef weak
#define weak __attribute__((__weak__))
#endif

#endif

/* alloca.h — stack allocation via the compiler builtin (picolibc ships no alloca.h). */
#ifndef _NANOS_ALLOCA_H
#define _NANOS_ALLOCA_H
#include <stddef.h>
#ifndef alloca
#define alloca(n) __builtin_alloca(n)
#endif
#endif

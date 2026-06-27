/* stb_impl.c — the single translation unit that emits the stb_truetype implementation for the
 * NanOS userland font engine (nwfont.c). Public-domain stb_truetype v1.26. Freestanding config:
 * heap via libc malloc/free, asserts off, math via picolibc. Other TUs include stb_truetype.h
 * for declarations only (no STB_TRUETYPE_IMPLEMENTATION). */
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_assert(x) ((void) 0)
#define STBTT_malloc(sz, u) ((void) (u), malloc(sz))
#define STBTT_free(p, u)    ((void) (u), free(p))
#include "stb_truetype.h"

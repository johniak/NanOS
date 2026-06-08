// Host implementations of freestanding kernel primitives, so the storage stack
// can be compiled and run natively under the test harness.
//
// Memory (malloc/free/realloc, global new/delete) and string.h come from libc by
// simply NOT linking memory_manager.cpp / string_funcs.cpp into the test binary.
// The real MI Console.cpp is compiled in the test build; only the arch console
// SINK (<arch/console.h>) needs a host stand-in — it normally writes VGA memory.
#include <arch/console.h>
#include <arch/input.h>
#include "memory_manager.h"
#include <cstdio>

// memory_manager.h declares malloc/free/realloc/calloc with C++ linkage (no
// extern "C"), so libc's C-linkage versions don't satisfy them. Provide the
// matching C++ symbols here, forwarding to the libc allocator via builtins
// (no <cstdlib> include, so no conflicting C-linkage redeclaration).
void* malloc(size_t n) { return __builtin_malloc(n); }
void free(void* p) { __builtin_free(p); }
void* realloc(void* p, size_t n) { return __builtin_realloc(p, n); }
void* calloc(size_t a, size_t b) { return __builtin_calloc(a, b); }

// Arch console sink stand-in: route glyphs to stdout, ignore cursor/clear.
namespace arch {
void consolePutChar(char c) { putchar(c); }
void consoleClear() {}
void consoleSetCursor(unsigned, unsigned) {}
void consoleInit() {}

// No real keyboard under the host harness: console reads are immediate EOF (this
// preserves the pre-blocking-stdin behaviour the Syscalls tests expect).
int inputRead(char*, unsigned, int) { return 0; }
void inputSetRaw(int) {}

// Scheduler arch primitives are hardware (context switch / timer); the host harness
// only exercises the pure round-robin logic, so these are no-op stubs.
unsigned archTaskBootstrap(unsigned char*, unsigned) { return 0; }
unsigned archKernelCr3() { return 0; }
void archTimerInit(unsigned) {}
void setKernelStack(unsigned) {}
void halt_or_hlt() {}
}
extern "C" void archContextSwitch(unsigned*, unsigned) {}   // C linkage (see arch/sched.h)

// Host implementations of freestanding kernel primitives, so the storage stack
// can be compiled and run natively under the test harness.
//
// Memory (malloc/free/realloc, global new/delete) and string.h come from libc by
// simply NOT linking memory_manager.cpp / string_funcs.cpp into the test binary.
// Only Console needs a host stand-in: it normally writes to VGA memory.
#include "Console.h"
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

namespace kernel {

unsigned short Console::cursorX = 0;
unsigned short Console::cursorY = 0;
volatile unsigned short* Console::videoram = 0;

void Console::scroll() {}
void Console::moveCursor() {}
void Console::goToXY(unsigned short, unsigned short) {}

void Console::write(char c) { putchar(c); }
void Console::write(int d) { printf("%d", d); }
void Console::write(const char* text) { fputs(text, stdout); }
void Console::writeHex(int hex) { printf("0x%X", hex); }
void Console::writeLine(char c) { printf("%c\n", c); }
void Console::writeLine(const char* line) { printf("%s\n", line); }
void Console::writeLine(int line) { printf("%d\n", line); }
void Console::clearScreen() {}

char* Console::itoa(int i, int base) {
	static char buf[34];
	if (base == 16) snprintf(buf, sizeof(buf), "%x", i);
	else snprintf(buf, sizeof(buf), "%d", i);
	return buf;
}

}

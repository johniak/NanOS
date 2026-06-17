/*
 * memory_manager.cpp
 *
 *  Created on: Feb 5, 2014
 *      Author: johniak
 */

#include <string.h>
#include "Heap.h"
#include "memory_manager.h"
#include <arch/console.h>
#include <arch/cpu.h>

// The kernel byte heap: a real free-list allocator (Heap) that reclaims and coalesces,
// replacing the old bump allocator whose free() was a no-op. heapInit() lays out the
// arena over the reserved heap region [base, base+size); it MUST run before the first
// malloc (called from mmuInitKernel, which knows topOfRam and runs before any `new`).
static kernel::Heap g_heap;

// Wired into Heap::onCorruption: the allocator detected a smashed boundary tag / free-list link.
// Print what + where directly to the console sink (no malloc, no formatting deps) and halt — a
// clean, located stop beats marching on through corrupted heap metadata.
static void heapPutStr(const char* s) { for (; *s; s++) arch::consolePutChar(*s); }
static void heapPutHex(uintptr_t v) {
	arch::consolePutChar('0'); arch::consolePutChar('x');
	// Print every nibble of a pointer-wide value (8 hex on i686, 16 on x86_64).
	for (int i = (int) (sizeof(uintptr_t) * 8) - 4; i >= 0; i -= 4) {
		int d = (int) ((v >> i) & 0xF);
		arch::consolePutChar((char) (d < 10 ? '0' + d : 'a' + d - 10));
	}
}
static void heapPanic(const char* what, uintptr_t off, uintptr_t hdr, uintptr_t ftr) {
	arch::cpuDisableInterrupts();
	heapPutStr("\n*** HEAP CORRUPTION: ");
	heapPutStr(what);
	heapPutStr(" off="); heapPutHex(off);
	heapPutStr(" hdr="); heapPutHex(hdr);
	heapPutStr(" ftr="); heapPutHex(ftr);
	heapPutStr(" ***\n");
	for (;;) arch::cpuHalt();
}

void heapInit(void* base, size_t size) {
	g_heap.init(base, size);
	kernel::Heap::onCorruption(heapPanic);
}

size_t heapTotalBytes(void) { return g_heap.totalBytes(); }
size_t heapFreeBytes(void)  { return g_heap.freeBytes(); }

void *malloc(size_t size) {
	return g_heap.alloc(size);
}
void *calloc(size_t nmeb, size_t size) {
	size_t total;
	if (__builtin_mul_overflow(nmeb, size, &total))
		return 0;                                  // reject the overflow
	void* ptr = g_heap.alloc(total);
	if (ptr)
		memset(ptr, 0, total);
	return ptr;
}
void free(void *ptr) {
	g_heap.free(ptr);
}
void *realloc(void *ptr, size_t size) {
	return g_heap.realloc(ptr, size);
}


void *operator new(size_t size)
{
    return malloc(size);
}

void *operator new[](size_t size)
{
    return malloc(size);
}

void operator delete(void *p)
{
    free(p);
}

void operator delete[](void *p)
{
    free(p);
}

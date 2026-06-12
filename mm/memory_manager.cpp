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
static void heapPutHex(unsigned v) {
	arch::consolePutChar('0'); arch::consolePutChar('x');
	for (int i = 28; i >= 0; i -= 4) {
		int d = (v >> i) & 0xF;
		arch::consolePutChar((char) (d < 10 ? '0' + d : 'a' + d - 10));
	}
}
static void heapPanic(const char* what, unsigned off, unsigned hdr, unsigned ftr) {
	arch::cpuDisableInterrupts();
	heapPutStr("\n*** HEAP CORRUPTION: ");
	heapPutStr(what);
	heapPutStr(" off="); heapPutHex(off);
	heapPutStr(" hdr="); heapPutHex(hdr);
	heapPutStr(" ftr="); heapPutHex(ftr);
	heapPutStr(" ***\n");
	for (;;) arch::cpuHalt();
}

void heapInit(void* base, unsigned size) {
	g_heap.init(base, size);
	kernel::Heap::onCorruption(heapPanic);
}

unsigned heapTotalBytes(void) { return g_heap.totalBytes(); }
unsigned heapFreeBytes(void) { return g_heap.freeBytes(); }

void *malloc(size_t size) {
	return g_heap.alloc((unsigned) size);
}
void *calloc(size_t nmeb, size_t size) {
	unsigned total = (unsigned) nmeb * (unsigned) size;
	void* ptr = g_heap.alloc(total);
	if (ptr)
		memset(ptr, 0, total);
	return ptr;
}
void free(void *ptr) {
	g_heap.free(ptr);
}
void *realloc(void *ptr, size_t size) {
	return g_heap.realloc(ptr, (unsigned) size);
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

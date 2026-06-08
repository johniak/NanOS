/*
 * memory_manager.cpp
 *
 *  Created on: Feb 5, 2014
 *      Author: johniak
 */

#include <string.h>
#include "Heap.h"
#include "memory_manager.h"

// The kernel byte heap: a real free-list allocator (Heap) that reclaims and coalesces,
// replacing the old bump allocator whose free() was a no-op. heapInit() lays out the
// arena over the reserved heap region [base, base+size); it MUST run before the first
// malloc (called from mmuInitKernel, which knows topOfRam and runs before any `new`).
static kernel::Heap g_heap;

void heapInit(void* base, unsigned size) {
	g_heap.init(base, size);
}

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

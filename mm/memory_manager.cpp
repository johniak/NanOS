/*
 * memory_manager.cpp
 *
 *  Created on: Feb 5, 2014
 *      Author: johniak
 */

#include <string.h>

// Bump allocator: hand out memory linearly from memory_heap_top, never reclaiming
// (free is a no-op). Each block carries an 8-byte header storing its size, so realloc
// can copy the old contents and grow without a side table. The previous design tracked
// every allocation in a fixed alocated[100000] array; under a heavy workload (Doom does
// thousands of file reads, each resolving a path into several Strings) that array
// overflowed and corrupted memory. The size header removes the array entirely.
void* memory_heap_top=(void*)123456789;

void *malloc(size_t size){
	// Reserve a header word, then 8-align the payload; store the size at payload[-4].
	unsigned base = (unsigned) memory_heap_top;
	unsigned payload = (base + 8 + 7) & ~7u;
	*((unsigned*) (payload - 4)) = (unsigned) size;
	memory_heap_top = (void*) (payload + size);
	return (void*) payload;
}
void *calloc(size_t nmeb, size_t size){
	unsigned total = (unsigned) nmeb * (unsigned) size;
	void* ptr = malloc(total);
	memset(ptr, 0, total);
	return ptr;
}
void free(void *ptr){
	(void) ptr;   // bump allocator does not reclaim
}
void *realloc(void *ptr, size_t size){
	// C standard: realloc(NULL, size) == malloc(size).
	if(!ptr)
		return malloc(size);
	unsigned oldLen = *((unsigned*) ((char*) ptr - 4));
	void* newPtr = malloc(size);
	unsigned copy = oldLen < (unsigned) size ? oldLen : (unsigned) size;
	memcpy(newPtr, ptr, copy);
	return newPtr;
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

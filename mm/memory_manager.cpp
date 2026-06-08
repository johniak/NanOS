/*
 * memory_manager.cpp
 *
 *  Created on: Feb 5, 2014
 *      Author: johniak
 */

#include <string.h>

void* memory_heap_top=(void*)123456789;

struct AlocatedStruct{
	void* start;
	int lenght;
};

AlocatedStruct alocated[100000];
int allocatedIndex=0;
void *malloc(size_t size){
	void* actual=memory_heap_top;
	alocated[allocatedIndex].start=actual;
	alocated[allocatedIndex].lenght=size;
	unsigned address=((int)memory_heap_top);
	address+=size;
	memory_heap_top=(void*)address;
	allocatedIndex++;
	return actual;
}
void *calloc(size_t nmeb, size_t size){
	void* ptr= malloc(nmeb*size);
	memset(ptr,0,size);
	return ptr;
}
void free(void *ptr){
	//hehe
}
void *realloc(void *ptr, size_t size){
	// C standard: realloc(NULL, size) == malloc(size).
	if(!ptr)
		return malloc(size);
	// Find the old allocation's length; 0 if we never recorded it (so we never copy a
	// garbage length from an uninitialized struct — the original bug this fixes).
	int oldLen = 0;
	for(int i=0;i<allocatedIndex;i++){
		if(alocated[i].start==ptr){
			oldLen = alocated[i].lenght;
			break;
		}
	}
	void* newPtr = malloc(size);
	unsigned copy = (unsigned) oldLen < (unsigned) size ? (unsigned) oldLen : (unsigned) size;
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

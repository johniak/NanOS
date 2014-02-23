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
	AlocatedStruct al;
	for(int i=0;i<allocatedIndex;i++){
		if(alocated[i].start==ptr){
			al=alocated[i];
			break;
		}
	}
	void* newPtr =malloc(size);
	memcpy(newPtr,ptr,al.lenght);
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

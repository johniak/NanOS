/*
 * memory_manager.h
 *
 *  Created on: Feb 5, 2014
 *      Author: johniak
 */

#include <string.h>
#ifndef MEMORY_MANAGER_H_
#define MEMORY_MANAGER_H_
void *malloc(size_t size);
void *calloc(size_t nmeb, size_t size);
void free(void *ptr);
void *realloc(void *ptr, size_t size);

// Lay out the kernel byte heap over [base, base+size). Must be called once, before the
// first malloc (the arch MMU bring-up does this once it knows the top of RAM).
void heapInit(void *base, unsigned size);





#endif /* MEMORY_MANAGER_H_ */

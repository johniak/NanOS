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





#endif /* MEMORY_MANAGER_H_ */

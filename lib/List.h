/*
 * List.h
 *
 *  Created on: Feb 5, 2014
 *      Author: johniak
 */

#include "memory_manager.h"

#ifndef LIST_H_
#define LIST_H_

#define S (String)

template<class T>
class List {
	T* array;
	size_t capacity;
	size_t capacityInc;
	size_t count;
public:
	List() :
			capacity(10), capacityInc(10), count(0) {
		// Use malloc (not new[]) so allocation/realloc/free are one consistent
		// family — new[] for non-trivial T adds an array cookie that free() and
		// realloc() do not understand.
		array = (T*) malloc(sizeof(T) * capacity);
	}
	void add(T item) {
		insert(count, item);
	}
	void insert(size_t index, T item) {
		if (count + 1 > capacity) {
			increaseCapacity();
		}
		if (index < count)
			memcpy(array + index + 1, array + index,
					(count - index) * sizeof(T));
		array[index] = item;
		count++;
	}
	void increaseCapacity() {
		capacity += capacityInc;
		// realloc already frees/moves the old block; the previous free(array)
		// here was a double free.
		array = (T*) realloc((void*) array, capacity * sizeof(T));
	}
	void removeAt(size_t index) {
		// (count - index - 1) is the number of trailing elements to shift left; the
		// old (count - index + 1) over-copied by two slots (an off-by-one bug).
		if (index + 1 < count) {
			memcpy(array + index, array + index + 1,
					(count - index - 1) * sizeof(T));
			count--;
			return;
		}
		if (index + 1 == count) {
			count--;
		}
	}
	size_t getCount() {
		return count;
	}
	T& operator[](size_t index) {
		return array[index];
	}
	virtual ~List() {
		free(array);
	}
};
#endif /* LIST_H_ */

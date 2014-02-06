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
	int capacity;
	int capacityInc;
	int count;
public:
	List() :
			capacity(10), capacityInc(10), count(0) {
		array = new T[capacity];
	}
	void add(T item) {
		insert(count, item);
	}
	void insert(int index, T item) {
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
		T* tmp = (T*) realloc((void*) array, capacity* sizeof(T));
		free(array);
		array = tmp;
	}
	void removeAt(int index) {
		if (index < count - 1) {
			memcpy(array + index, array + index + 1,
					(count - index + 1) * sizeof(T));
			count--;
			return;
		}
		if (index == count - 1) {
			count--;
		}
	}
	int getCount() {
		return count;
	}
	T& operator[](const int index) {
		return array[index];
	}
	virtual ~List() {
		free(array);
	}
};
#endif /* LIST_H_ */

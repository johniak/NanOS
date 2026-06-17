/*
 * String.h
 *
 *  Created on: Feb 5, 2014
 *      Author: johniak
 */
#include <string.h>
#include "memory_manager.h"
#include "List.h"
#include "Console.h"
#ifndef STRING_H_
#define STRING_H_

class String {
	char* textArray;
	size_t length;
public:
	String();
	String(const char* text) {
		length = strlen(text);
		textArray = (char*) malloc(length + 1);
		memcpy(textArray, text, length);
		textArray[length] = 0;
	}
	String(int dec) {
		char* text = itoa(dec, 10);
		length = strlen(text);
		textArray = (char*) malloc(length + 1);
		memcpy(textArray, text, length);
		textArray[length] = 0;
	}

	// Value semantics: each String owns its buffer (deep copy on copy/assign, freed in
	// the destructor). Without this, copies would share a buffer and the now-real free()
	// would double-free or use-after-free across the many pass-by-value call sites.
	String(const String& o) {
		length = o.length;
		if (o.textArray) {
			textArray = (char*) malloc(length + 1);
			memcpy(textArray, o.textArray, length);
			textArray[length] = 0;
		} else {
			textArray = 0;
		}
	}
	String& operator=(const String& o) {
		if (this == &o)
			return *this;
		free(textArray);
		length = o.length;
		if (o.textArray) {
			textArray = (char*) malloc(length + 1);
			memcpy(textArray, o.textArray, length);
			textArray[length] = 0;
		} else {
			textArray = 0;
		}
		return *this;
	}

	void append(const String& str) {
		size_t totalLenght = length + str.length;
		// realloc handles textArray==0 (acts as malloc) and frees the old buffer itself,
		// so there is no separate free() (that was a double-free under a real allocator).
		textArray = (char*) realloc(textArray, totalLenght + 1);
		if (str.textArray)
			memcpy(textArray + length, str.textArray, str.length);
		textArray[totalLenght] = 0;
		length = totalLenght;
	}

	size_t getLenght() {       // keep the (historic) name; widen the type
		return length;
	}
	char* itoa(int value, int base) {
#define INT_DIGITS 19
		static char result[32] = { 0 };
		// check that the base if valid
		if (base < 2 || base > 36) {
			*result = '\0';
			return result;
		}

		char* ptr = result, *ptr1 = result, tmp_char;
		int tmp_value;

		do {
			tmp_value = value;
			value /= base;
			*ptr++ =
					"zyxwvutsrqponmlkjihgfedcba9876543210123456789abcdefghijklmnopqrstuvwxyz"[35
							+ (tmp_value - value * base)];
		} while (value);

		// Apply negative sign
		if (tmp_value < 0)
			*ptr++ = '-';
		*ptr-- = '\0';
		while (ptr1 < ptr) {
			tmp_char = *ptr;
			*ptr-- = *ptr1;
			*ptr1++ = tmp_char;
		}
		return result;

	}
	int indexOf(String str) {
		return indexOf(str, 0);
	}

	int indexOf(String str, int start) {
		if ((size_t) start >= length)
			return -1;
		char* ptr = strstr(textArray + start, str.textArray);
		if (ptr == 0)
			return -1;
		return (int) (ptr - textArray);
	}
	int compareTo(String str){
		return strcmp(textArray,str.textArray);
	}
	// (split/substring removed: they returned List<String>/String by value through a
	// container that can't hold value types safely, and the only user — ext path lookup —
	// now parses components in place. See ExtFilesystem::getInodeByPath.)
	bool startsWith(String str){
		return indexOf(str)==0;
	}
	operator char*() {
		return textArray;
	}
	String operator+(String str) {
		this->append(str);
		return *this;
	}
	String operator+(char* str) {
		this->append(str);
		return *this;
	}
	String operator+(int dec) {
		this->append(String(dec));
		return *this;
	}
	char operator[](const int index) {
		return textArray[index];
	}
	virtual ~String();
};
String operator+(String str1, const char* str2);
#endif /* STRING_H_ */

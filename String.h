/*
 * String.h
 *
 *  Created on: Feb 5, 2014
 *      Author: johniak
 */
#include <string.h>
#include "memory_manager.h"
#ifndef STRING_H_
#define STRING_H_

class String {
	char* textArray;
	int length;
public:
	String();
	String(const char* text) {
		length = strlen(text);
		textArray = (char*) malloc(length + 1);
		memcpy(textArray, text, length);
		textArray[length] = 0;
	}
	String(int dec) {
		char* text=itoa(dec,10);
		length = strlen(text);
		textArray = (char*) malloc(length + 1);
		memcpy(textArray, text, length);
		textArray[length] = 0;
	}

	void append(String str) {
		int totalLenght = length + str.length;
		if (length == 0) {
			textArray = (char*) malloc(length);
		} else {
			char* tmp = (char*) realloc(textArray, totalLenght + 1);
			free(textArray);
			textArray = tmp;
		}
		memcpy(textArray + length, str.textArray, str.length);
		length = totalLenght;
	}

	int getLenght() {
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
	virtual ~String();
};
String operator+(String str1, const char* str2);
#endif /* STRING_H_ */

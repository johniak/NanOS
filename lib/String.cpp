/*
 * String.cpp
 *
 *  Created on: Feb 5, 2014
 *      Author: johniak
 */

#include "String.h"

String::String() {
	length = 0;
	textArray = 0;   // empty String owns no buffer (free(0) in the dtor is a no-op)
}

String::~String() {
	free(textArray);   // value semantics: each String frees its own buffer
}

String operator+(String str1,const char* str2)
{
    return  str1+String(str2);
}

String operator+(String str1,int dec)
{
    return  str1+String(dec);
}

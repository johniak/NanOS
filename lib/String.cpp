/*
 * String.cpp
 *
 *  Created on: Feb 5, 2014
 *      Author: johniak
 */

#include "String.h"

String::String() {
	length=0;

}

String::~String() {
	// TODO Auto-generated destructor stub
}

String operator+(String str1,const char* str2)
{
    return  str1+String(str2);
}

String operator+(String str1,int dec)
{
    return  str1+String(dec);
}

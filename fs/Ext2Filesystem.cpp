/*
 * Ext2Filesystem.cpp
 *
 *  Created on: Feb 4, 2014
 *      Author: johniak
 */

#include "Ext2Filesystem.h"

namespace kernel {
int ceil(float num) {
    int inum = (int)num;
    if (num == (float)inum) {
        return inum;
    }
    return inum + 1;
}
} /* namespace kernel */

#include "ExtFilesystem.h"

namespace kernel {
int ceil(float num) {
	int inum = (int) num;
	if (num == (float) inum) {
		return inum;
	}
	return inum + 1;
}
} /* namespace kernel */

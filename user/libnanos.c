#include "libnanos.h"
#include "NxFormat.h"

// Import Address Table: the loader fills each slot with the resolved export.
void* __nx_iat[NX_NIMPORTS];

// Import descriptor the loader reads (name -> which IAT slot to patch).
__attribute__((section(".nximports"), used))
const NxImport __nx_imports[NX_NIMPORTS] = {
	{ (unsigned) "write", (unsigned) &__nx_iat[0] },
	{ (unsigned) "read",  (unsigned) &__nx_iat[1] },
	{ (unsigned) "open",  (unsigned) &__nx_iat[2] },
	{ (unsigned) "close", (unsigned) &__nx_iat[3] },
	{ (unsigned) "exit",  (unsigned) &__nx_iat[4] },
};

int write(int fd, const void* b, unsigned n) {
	return ((int (*)(int, const void*, unsigned)) __nx_iat[0])(fd, b, n);
}
int read(int fd, void* b, unsigned n) {
	return ((int (*)(int, void*, unsigned)) __nx_iat[1])(fd, b, n);
}
int open(const char* p, int f) {
	return ((int (*)(const char*, int)) __nx_iat[2])(p, f);
}
int close(int fd) {
	return ((int (*)(int)) __nx_iat[3])(fd);
}
void exit(int c) {
	((void (*)(int)) __nx_iat[4])(c);
	for (;;) {}
}

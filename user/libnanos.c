#include "libnanos.h"
#include "NxFormat.h"
#include "SyscallNr.h"

// Issue a Linux-style syscall via int 0x80 (nr in eax, args in ebx/ecx/edx).
static inline int syscall3(int nr, int a0, int a1, int a2) {
	int ret;
	__asm__ __volatile__("int $0x80"
			: "=a"(ret)
			: "a"(nr), "b"(a0), "c"(a1), "d"(a2)
			: "memory");
	return ret;
}

// Import Address Table: the loader fills each slot with the resolved export.
// Only exit() still uses it (step B1); the data syscalls now trap via int 0x80.
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
	return syscall3(SYS_write, fd, (int) b, (int) n);
}
int read(int fd, void* b, unsigned n) {
	return syscall3(SYS_read, fd, (int) b, (int) n);
}
int open(const char* p, int f) {
	return syscall3(SYS_open, (int) p, f, 0);
}
int close(int fd) {
	return syscall3(SYS_close, fd, 0, 0);
}
void exit(int c) {
	((void (*)(int)) __nx_iat[4])(c);
	for (;;) {}
}

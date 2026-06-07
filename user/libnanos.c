#include "libnanos.h"
#include "SyscallNr.h"

// All NanOS syscalls go through int 0x80 (nr in eax, args in ebx/ecx/edx). This
// is the userland's thin syscall layer (the ntdll/glibc analog). The IAT /
// import-by-name path is gone — reserved for a future .ndl dynamic library.
static inline int syscall3(int nr, int a0, int a1, int a2) {
	int ret;
	__asm__ __volatile__("int $0x80"
			: "=a"(ret)
			: "a"(nr), "b"(a0), "c"(a1), "d"(a2)
			: "memory");
	return ret;
}

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
	syscall3(SYS_exit, c, 0, 0);
	for (;;) {}
}

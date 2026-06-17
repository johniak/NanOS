#include "libnanos.h"
#include "SyscallNr.h"

// The userland's thin syscall layer (the ntdll/glibc analog). The IAT / import-by-name path
// is gone — reserved for a future .ndl dynamic library. The trap mechanism is arch-specific:
// x86_64 uses the `syscall` instruction (nr in rax, args in rdi/rsi/rdx, clobbers rcx/r11),
// i386 uses int 0x80 (nr in eax, args in ebx/ecx/edx). Args/types are `long` on x86_64 so a
// 64-bit pointer is passed whole (vs `int` on i686, where pointers are 32-bit).
#if defined(__x86_64__)
static inline long syscall3(long nr, long a0, long a1, long a2) {
	long ret;
	__asm__ __volatile__("syscall"
			: "=a"(ret)
			: "a"(nr), "D"(a0), "S"(a1), "d"(a2)
			: "rcx", "r11", "memory");
	return ret;
}
#else
static inline int syscall3(int nr, int a0, int a1, int a2) {
	int ret;
	__asm__ __volatile__("int $0x80"
			: "=a"(ret)
			: "a"(nr), "b"(a0), "c"(a1), "d"(a2)
			: "memory");
	return ret;
}
#endif

int write(int fd, const void* b, unsigned n) { return (int) syscall3(SYS_write, fd, (long) b, n); }
int read(int fd, void* b, unsigned n)         { return (int) syscall3(SYS_read, fd, (long) b, n); }
int open(const char* p, int f)                { return (int) syscall3(SYS_open, (long) p, f, 0); }
int close(int fd)                             { return (int) syscall3(SYS_close, fd, 0, 0); }
void exit(int c)                              { syscall3(SYS_exit, c, 0, 0); for (;;) {} }

/*
 * Syscall.h
 *
 * Linux-style syscall implementation over the VFS (the host-testable core).
 * The int 0x80 dispatch glue lives in SyscallDispatch.{h,cpp}.
 */
#include "Vfs.h"
#include "String.h"
#ifndef SYSCALL_H_
#define SYSCALL_H_

#include "SyscallNr.h"   // SYS_* numbers (shared with userland, plain C)

namespace kernel {

// errno values returned (negated) on error.
#define ENOENT 2
#define EBADF 9
#define EINVAL 22
#define EROFS 30

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef int (*ConsoleWriteFn)(const char* buf, unsigned len);

struct LinuxStat {
	unsigned st_mode;   // S_IFREG (0x8000) / S_IFDIR (0x4000) | perms
	unsigned st_size;
};

class Syscalls {
	struct Fd {
		bool used;
		bool isConsole;
		String path;
		unsigned offset;
		unsigned size;
	};
	static const int MAXFD = 32;
	Fd fds[MAXFD];
	Vfs* vfs;
	ConsoleWriteFn consoleWrite;
	bool exited;
	int exitCode;

	bool valid(int fd) {
		return fd >= 0 && fd < MAXFD && fds[fd].used;
	}
public:
	Syscalls(Vfs* vfs, ConsoleWriteFn cw);
	int open(String path, int flags);
	int close(int fd);
	int read(int fd, void* buf, unsigned n);
	int write(int fd, const void* buf, unsigned n);
	int lseek(int fd, int off, int whence);
	int fstat(int fd, LinuxStat* out);
	int getdents64(int fd, void* buf, unsigned n);
	void exit(int code);
	bool hasExited() { return exited; }
	int code() { return exitCode; }
	// Clear the exit state so the same Syscalls instance can run another program.
	void resetForRun() { exited = false; exitCode = 0; }
};

} /* namespace kernel */

#endif /* SYSCALL_H_ */

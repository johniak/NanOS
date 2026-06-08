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
#define EAGAIN 11
#define EINVAL 22
#define EROFS 30

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

// fcntl commands + the open/status flags we honor (picolibc/newlib values — userland
// passes these straight through, so they MUST match <fcntl.h> on the i686-elf target).
#define F_GETFL 3
#define F_SETFL 4
#define O_NONBLOCK 0x4000
#define O_CREAT 0x40
#define O_TRUNC 0x200

typedef int (*ConsoleWriteFn)(const char* buf, unsigned len);

struct LinuxStat {
	unsigned st_mode;   // S_IFREG (0x8000) / S_IFDIR (0x4000) | perms
	unsigned st_size;
	unsigned st_nlink;
	unsigned st_uid;
	unsigned st_gid;
	unsigned st_mtime;
	unsigned st_ino;
};

// Kernel mirror of picolibc's i686 struct timespec: a 64-bit time_t (tv_sec, 8 bytes
// at offset 0) followed by a 32-bit tv_nsec at offset 8 (total 12 bytes). Defined here
// rather than pulled from <time.h> so the host test compiles without libc clashes. The
// layout MUST match picolibc or the kernel reads/writes the wrong field offsets.
struct KTimespec {
	long long tv_sec;
	int tv_nsec;
};

class Syscalls {
	struct Fd {
		bool used;
		bool isConsole;
		String path;
		unsigned offset;
		unsigned size;
		unsigned flags;     // file status flags (O_NONBLOCK); set via fcntl(F_SETFL)
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
	int stat(String path, LinuxStat* out);
	int fstat(int fd, LinuxStat* out);
	int unlink(String path);              // remove a file (writable fs only)
	int mkdir(String path, int mode);     // create a directory (writable fs only)
	int getdents64(int fd, void* buf, unsigned n);
	int ioctl(int fd, unsigned cmd, void* arg);
	int fcntl(int fd, int cmd, int arg);   // F_GETFL/F_SETFL (O_NONBLOCK)
	int mmapInfo(int fd, unsigned* physOut, unsigned* lenOut);   // for SYS_mmap of a device
	// Time. clockGettime fills `out` from a monotonic tick count (1000 Hz => ms); the
	// dispatch supplies Scheduler::ticks(). nanosleepMs converts a requested timespec to
	// the number of whole milliseconds to block (rounding up); the actual blocking loop
	// lives in the dispatch (it needs the scheduler/IRQs).
	int clockGettime(int clkId, unsigned ticks, KTimespec* out);
	unsigned nanosleepMs(const KTimespec* req);
	void exit(int code);
	bool hasExited() { return exited; }
	int code() { return exitCode; }
	// Clear the exit state so the same Syscalls instance can run another program.
	void resetForRun() { exited = false; exitCode = 0; }
};

} /* namespace kernel */

#endif /* SYSCALL_H_ */

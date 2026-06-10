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
#include "Pipe.h"
#include "CharDevice.h"  // POLLIN/POLLOUT/... (single source) + device interface
#include "Termios.h"     // console terminal settings carried by TCGETS/TCSETS

namespace kernel {

// errno values returned (negated) on error.
#define ENOENT 2
#define E2BIG 7
#define EBADF 9
#define EAGAIN 11
#define EINVAL 22
#define EROFS 30
#define EMFILE 24
#define EPIPE 32
#define ENOTDIR 20
#define ERANGE 34

// poll(2) event/revent bits live in CharDevice.h (included above) — single source.
struct PollFd { int fd; short events; short revents; };

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

// fcntl commands + the open/status flags we honor (picolibc/newlib values for i686-elf —
// userland passes these straight through, so they MUST match <fcntl.h> on the target,
// verified against /opt/picolibc/i686-elf/include/sys/_default_fcntl.h).
#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_DUPFD_CLOEXEC 14
#define FD_CLOEXEC 1
#define O_ACCMODE 3            // O_RDONLY(0) | O_WRONLY(1) | O_RDWR(2)
#define O_CREAT 0x40
#define O_TRUNC 0x200
#define O_APPEND 0x400
#define O_NONBLOCK 0x4000
#define O_CLOEXEC 0x40000

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
		unsigned flags;     // file status flags (O_NONBLOCK/O_APPEND); set at open / fcntl(F_SETFL)
		bool cloexec;       // FD_CLOEXEC: close this fd on execve (fcntl F_SETFD / O_CLOEXEC)
		Pipe* pipe;         // non-null => this fd is one end of a pipe
		bool pipeWrite;     // which end (write end if true, read end otherwise)
	};
	static const int MAXFD = 128;   // per-process fd table (was an artificial 32; heap-backed)
	Fd fds[MAXFD];
	Vfs* vfs;
	ConsoleWriteFn consoleWrite;
	Termios consoleTermios;   // real terminal settings for the console fds (0/1/2)
	String m_cwd;             // current working directory (absolute); inherited on fork, kept on execve
	bool exited;
	int exitCode;

	bool valid(int fd) {
		return fd >= 0 && fd < MAXFD && fds[fd].used;
	}
	int allocFd(int from = 0);     // lowest free descriptor >= from, or -EMFILE
	void shareInto(int dst, int src);   // make dst alias src's backing (for dup/dup2)
public:
	Syscalls(Vfs* vfs, ConsoleWriteFn cw);
	Syscalls(const Syscalls& o);   // fork: dup the fd table, bumping pipe-end refcounts
	~Syscalls();                   // process exit: close fds, dropping pipe-end refcounts
	int open(String path, int flags);
	int close(int fd);
	int read(int fd, void* buf, unsigned n);
	int write(int fd, const void* buf, unsigned n);
	int lseek(int fd, int off, int whence);
	int stat(String path, LinuxStat* out);
	int lstat(String path, LinuxStat* out);                 // stat the link itself (no follow)
	int readlink(String path, char* buf, unsigned size);    // a symlink's target, or -errno
	int fstat(int fd, LinuxStat* out);
	int unlink(String path);              // remove a file (writable fs only)
	int mkdir(String path, int mode);     // create a directory (writable fs only)
	// Working directory (Linux model: kernel-tracked per process, inherited by fork, kept
	// across execve). chdir validates the target is a directory; getcwd copies it out.
	// resolvePath turns any path (relative -> against the cwd) into a clean absolute path,
	// normalising '.'/'..'; every path-taking syscall runs it, so programs need not resolve.
	int chdir(String path);
	int getcwd(char* buf, unsigned size);
	String resolvePath(String path);
	int getdents64(int fd, void* buf, unsigned n);
	int ioctl(int fd, unsigned cmd, void* arg);
	int fcntl(int fd, int cmd, int arg);   // F_GETFL/F_SETFL, F_GETFD/F_SETFD, F_DUPFD[_CLOEXEC]
	void closeCloexec();                   // close every FD_CLOEXEC descriptor (called at execve)
	void closeAll();                       // close every open descriptor (called at process exit,
	                                       // so pipe peers see EOF without waiting for the reap)
	// True when the console termios has canonical mode (ICANON) cleared, i.e. the line
	// discipline should be raw. The dispatch reads this after a console TCSETS to drive
	// arch::inputSetRaw, so tcsetattr(raw) actually switches the input mode.
	bool consoleRaw() { return (consoleTermios.c_lflag & TL_ICANON) == 0; }
	bool isConsoleFd(int fd) { return valid(fd) && fds[fd].isConsole; }
	// If `fd` refers to a tty (its device answers TIOCGPGRP), return that tty's foreground
	// process group; otherwise -1. The dispatch uses this to raise SIGTTIN on a background
	// process reading the controlling terminal.
	int ttyPgrp(int fd);
	// Pipes + descriptor duplication. pipe() fills out[0]=read end, out[1]=write end.
	// dup/dup2 alias an existing descriptor's backing (sharing a pipe end / file / console).
	int pipe(int out[2]);
	int dup(int fd);
	int dup2(int oldfd, int newfd);
	// Non-blocking poll scan: fills each pollfd's revents and returns the number of fds that
	// are ready (revents != 0). Blocking + timeout are handled by the dispatch.
	int pollScan(PollFd* pfds, int nfds);
	// Pipe readiness, for the dispatch's blocking loops (it owns the scheduler).
	bool fdReadable(int fd);
	bool fdWritable(int fd);
	bool isPipe(int fd) { return valid(fd) && fds[fd].pipe != 0; }
	bool nonblock(int fd) { return valid(fd) && (fds[fd].flags & O_NONBLOCK) != 0; }
	int mmapInfo(int fd, unsigned* physOut, unsigned* lenOut);   // for SYS_mmap of a device
	// Time. clockGettime fills `out` from a monotonic tick count (1000 Hz => ms); the
	// dispatch supplies Scheduler::ticks(). nanosleepMs converts a requested timespec to
	// the number of whole milliseconds to block (rounding up); the actual blocking loop
	// lives in the dispatch (it needs the scheduler/IRQs).
	// CLOCK_MONOTONIC (and default): seconds/nanos from `ticks` (1000 Hz => ms since boot).
	// CLOCK_REALTIME: wall clock = `realtimeSec` (from the RTC) + the sub-second tick part.
	int clockGettime(int clkId, unsigned ticks, unsigned realtimeSec, KTimespec* out);
	unsigned nanosleepMs(const KTimespec* req);
	void exit(int code);
	bool hasExited() { return exited; }
	int code() { return exitCode; }
	// Clear the exit state so the same Syscalls instance can run another program.
	void resetForRun() { exited = false; exitCode = 0; }
};

} /* namespace kernel */

#endif /* SYSCALL_H_ */

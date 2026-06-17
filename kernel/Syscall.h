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
#define EFAULT 14
#define EINVAL 22
#define EROFS 30
#define EMFILE 24
#define EPIPE 32
#define ENOTDIR 20
#define ERANGE 34
// socket errnos — MUST match the target C library (picolibc/newlib) numbering: the libc-glue
// passes the kernel's negated errno straight to picolibc `errno` (user/libc-glue reterr:
// errno = -r), so these are the values userland actually compares against. Verified against the
// sysroot <sys/errno.h>. (Previously Linux i686 values, which COLLIDE with newlib — e.g. Linux
// EINPROGRESS=115 is newlib ENETDOWN, so a non-blocking connect() returned errno 115 and a
// picolibc client like libcurl saw "network down" instead of "in progress" and aborted.)
#define EACCES 13
#define EMSGSIZE 122
#define EPROTONOSUPPORT 123
#define EOPNOTSUPP 95
#define EAFNOSUPPORT 106
#define EADDRINUSE 112
#define ENETUNREACH 114
#define ENOBUFS 105
#define EISCONN 127
#define ENOTCONN 128
#define ECONNREFUSED 111
#define EINPROGRESS 119
#define ENOTSOCK 108
#define ENXIO 6

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

struct Socket;   // net/Socket.h — a socket fd's backing (FAZA 9)

typedef int (*ConsoleWriteFn)(const char* buf, unsigned len);

typedef long long off_t;     // 64-bit signed file offset (x86_64 ABI)

// Kernel-internal stat buffer filled by stat/fstat and marshalled to userland by the libc-glue.
// Field WIDTHS match the x86_64 ABI: 64-bit st_ino/st_size/time. The exact on-wire field ORDER is
// owned by the userland stat marshaller (user/libc-glue) — coordinate any reorder with Plan 6/8.
struct LinuxStat {
	uint64_t st_ino;     // 64-bit inode number
	uint32_t st_mode;    // S_IF* | perms
	uint32_t st_nlink;
	uint32_t st_uid;
	uint32_t st_gid;
	uint64_t st_size;    // 64-bit off_t
	uint64_t st_blocks;  // 512-byte block count
	int64_t  st_mtime;   // 64-bit time_t
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
		off_t offset;       // 64-bit file position
		off_t size;         // 64-bit cached size
		unsigned flags;     // file status flags (O_NONBLOCK/O_APPEND); set at open / fcntl(F_SETFL)
		bool cloexec;       // FD_CLOEXEC: close this fd on execve (fcntl F_SETFD / O_CLOEXEC)
		Pipe* pipe;         // non-null => this fd is one end of a pipe
		bool pipeWrite;     // which end (write end if true, read end otherwise)
		Socket* sock;       // non-null => this fd is a socket (read/write/poll/close route to it)
		bool isChar;        // this fd is a char-device path (pty/etc): open/close are ref-counted
	};
	static const int MAXFD = 128;   // per-process fd table (was an artificial 32; heap-backed)
	Fd fds[MAXFD];
	Vfs* vfs;
	ConsoleWriteFn consoleWrite;
	Termios consoleTermios;   // real terminal settings for the console fds (0/1/2)
	String m_cwd;             // current working directory (absolute); inherited on fork, kept on execve
	unsigned m_umask;         // file-creation mask (default 022); applied by mkdir/creat
	unsigned m_uid, m_gid;    // real user/group id (process credentials; 0 = root)
	unsigned m_euid, m_egid;  // effective ids — used for permission checks; inherited on fork
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
	// On the last close of a pipe, the Pipe (and its embedded WaitQueue) is freed; *freedShared
	// is set true so the caller knows NOT to touch that queue afterwards (use-after-free guard).
	int close(int fd, bool* freedShared = nullptr);
	int read(int fd, void* buf, unsigned n);
	int write(int fd, const void* buf, unsigned n);
	off_t lseek(int fd, off_t off, int whence);
	int stat(String path, LinuxStat* out);
	int lstat(String path, LinuxStat* out);                 // stat the link itself (no follow)
	int readlink(String path, char* buf, unsigned size);    // a symlink's target, or -errno
	int fstat(int fd, LinuxStat* out);
	int unlink(String path);              // remove a file (writable fs only)
	int mkdir(String path, int mode);     // create a directory (writable fs only)
	int rmdir(String path);               // remove an empty directory
	int rename(String oldpath, String newpath);   // rename/move within one filesystem
	int link(String oldpath, String newpath);     // create a hard link
	int symlink(String target, String path);      // create a symbolic link (target stored as-is)

	// Metadata + size (Phase 4). The f* variants act on an open fd's path; truncate/ftruncate
	// also fix the cached size. chown's uid/gid of -1 (0xFFFFFFFF) leave that field unchanged.
	int chmod(String path, int mode);
	int fchmod(int fd, int mode);
	int chown(String path, int uid, int gid);
	int lchown(String path, int uid, int gid);
	int fchown(int fd, int uid, int gid);
	int truncate(String path, off_t length);
	int ftruncate(int fd, off_t length);
	int utimes(String path, unsigned atime, unsigned mtime);
	int utime(String path, const void* times);              // struct utimbuf{actime,modtime} or NULL
	int utimensat(int dirfd, String path, const void* times, int flags);  // timespec[2]/NULL + UTIME_*
	int access(String path, int mode);
	int faccessat(int dirfd, String path, int mode, int flags);
	int renameat2(int oldfd, String oldpath, int newfd, String newpath, int flags);  // NOREPLACE/EXCHANGE
	unsigned currentTime();                                 // wall-clock seconds (0 until a clock is set)

	// Process credentials. uid 0 (root) bypasses read/write permission checks. Inherited on fork.
	int getuid();
	int geteuid();
	int getgid();
	int getegid();
	int setuid(int uid);
	int setgid(int gid);
	// Permission test against the calling process's effective ids: `want` is r/w/x bits
	// (4/2/1). Returns 0 if allowed, -EACCES otherwise. Used by open().
	int permCheck(const FileStat& st, int want);
	int statfs(String path, void* buf);            // fills LinuxStatfs
	int fstatfs(int fd, void* buf);
	int fsync(int fd);
	int fchdir(int fd);
	int umask(int mask);                            // returns the previous mask
	int creat(String path, int mode);

	// dirfd-relative (*at) family. dirfd == AT_FDCWD uses the cwd; an absolute path ignores it.
	int openat(int dirfd, String path, int flags);
	int mkdirat(int dirfd, String path, int mode);
	int unlinkat(int dirfd, String path, int flags);   // AT_REMOVEDIR -> rmdir
	int renameat(int oldfd, String oldpath, int newfd, String newpath);
	int linkat(int oldfd, String oldpath, int newfd, String newpath, int flags);
	int symlinkat(String target, int newfd, String path);
	int readlinkat(int dirfd, String path, char* buf, unsigned size);
	int fchmodat(int dirfd, String path, int mode, int flags);
	int fchownat(int dirfd, String path, int uid, int gid, int flags);
	int fstatat(int dirfd, String path, LinuxStat* out, int flags);   // AT_SYMLINK_NOFOLLOW
	String resolveAt(int dirfd, String path, bool* badfd);             // join dirfd + path
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
	// The wait list a blocking read/write on `fd` should park on (the pipe's, or the backing
	// char device's), or 0 for fds that never block this way (regular files; the console, which
	// blocks inside arch::inputRead). Lets the dispatch sleep event-driven, not per tick.
	WaitQueue* fdWaitQueue(int fd);
	int mmapInfo(int fd, uint64_t* physOut, unsigned* lenOut);   // for SYS_mmap of a device

	// ---- Sockets (FAZA 9). Addresses cross the ABI as Linux sockaddr_in (family/port-BE/addr-BE).
	// The socket lives in the fd table (read/write/close/poll/dup/fork-refcount route to it).
	int sockSocket(int domain, int type, int protocol);                 // new fd, or -errno
	int sockBind(int fd, const void* sa, unsigned salen);
	int sockConnect(int fd, const void* sa, unsigned salen);            // 0 / -EINPROGRESS (TCP) / -errno
	int sockConnectResult(int fd);                                      // 0 done / -EINPROGRESS / -errno
	int sockListen(int fd, int backlog);
	int sockAccept(int fd, void* sa, unsigned* salen);                  // new fd, or -EAGAIN/-errno
	int sockSocketpair(int domain, int type, int protocol, int sv[2]); // AF_UNIX pre-connected pair
	int sockGetsockopt(int fd, int level, int name, void* val, unsigned* len);
	int sockSetsockopt(int fd, int level, int name, const void* val, unsigned len);
	int sockGetsockname(int fd, void* sa, unsigned* salen);
	int sockGetpeername(int fd, void* sa, unsigned* salen);
	int sockSendto(int fd, const void* buf, unsigned len, int flags, const void* sa, unsigned salen);
	int sockRecvfrom(int fd, void* buf, unsigned len, int flags, void* sa, unsigned* salen);
	int sockShutdown(int fd, int how);
	bool isSocketFd(int fd) { return valid(fd) && fds[fd].sock != 0; }
	int netIoctl(int fd, unsigned cmd, void* arg);                      // SIOCGIF*/SIOCADDRT/...
	// Time. clockGettime fills `out` from a monotonic tick count (1000 Hz => ms); the
	// dispatch supplies Scheduler::ticks(). nanosleepMs converts a requested timespec to
	// the number of whole milliseconds to block (rounding up); the actual blocking loop
	// lives in the dispatch (it needs the scheduler/IRQs).
	// CLOCK_MONOTONIC (and default): seconds/nanos from `ticks` (1000 Hz => ms since boot).
	// CLOCK_REALTIME: wall clock = `epochBaseSec` (the RTC sampled ONCE at boot) + the FULL
	// monotonic tick (seconds AND sub-second), so realtime stays monotonic across second
	// boundaries (a live-RTC seconds read mixed with tick sub-seconds made ping RTTs negative).
	int clockGettime(int clkId, unsigned ticks, unsigned epochBaseSec, KTimespec* out);
	unsigned nanosleepMs(const KTimespec* req);
	void exit(int code);
	bool hasExited() { return exited; }
	int code() { return exitCode; }
	// Clear the exit state so the same Syscalls instance can run another program.
	void resetForRun() { exited = false; exitCode = 0; }
};

} /* namespace kernel */

#endif /* SYSCALL_H_ */

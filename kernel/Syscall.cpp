#include "Syscall.h"
#include "List.h"
#include "string.h"
#include "Termios.h"
#include "Clock.h"
#include "Scheduler.h"
#include "Socket.h"     // FAZA 9: socket fd backing + the socket API
#include "Tcp.h"        // tcpListen/tcpAccept/tcpState for listen()/accept()
#include "Net.h"        // hton/ntoh + ipv4() for sockaddr marshalling
#include "NetDevice.h"  // net ioctls (SIOCGIF*)
#include "Route.h"      // SIOCADDRT
#include "Packet.h"     // AF_PACKET (sockaddr_ll marshalling, ifindex)
#include "Unix.h"       // AF_UNIX (sockaddr_un marshalling, socketpair)
#include <arch/input.h>

namespace kernel {

// Wall clock: a boot epoch (seeded once from the RTC) plus seconds since boot (the 1000 Hz
// scheduler tick). MI, so filesystem code can timestamp inodes without touching the arch.
static unsigned g_bootEpoch = 0;
void setBootEpoch(unsigned epochSeconds) { g_bootEpoch = epochSeconds; }
unsigned bootEpochSeconds() { return g_bootEpoch; }
unsigned wallClockSeconds() { return g_bootEpoch + Scheduler::ticks() / 1000u; }

// Canonical "cooked" terminal defaults, matching what a Linux tty starts with: line-based
// input (ICANON), echo on, signal keys on, CR->NL on input, NL->CRLF on output, and the
// standard control characters. tcgetattr() reads these; tcsetattr() replaces them.
static void initCookedTermios(Termios& t) {
	memset(&t, 0, sizeof(t));
	t.c_iflag = TI_ICRNL | TI_IXON;
	t.c_oflag = TO_OPOST | TO_ONLCR;
	t.c_cflag = 0;
	t.c_lflag = TL_ISIG | TL_ICANON | TL_ECHO | TL_ECHOE | TL_ECHOK;
	t.c_cc[VINTR] = 3;      // ^C
	t.c_cc[VQUIT] = 28;     // ^\ .
	t.c_cc[VERASE] = 0x7f;  // DEL
	t.c_cc[VKILL] = 21;     // ^U
	t.c_cc[VEOF] = 4;       // ^D
	t.c_cc[VSUSP] = 26;     // ^Z
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;
}

Syscalls::Syscalls(Vfs* vfs, ConsoleWriteFn cw) {
	this->vfs = vfs;
	this->consoleWrite = cw;
	this->exited = false;
	this->exitCode = 0;
	this->m_cwd = String("/");
	this->m_umask = 0022;
	this->m_uid = this->m_gid = this->m_euid = this->m_egid = 0;   // start as root
	initCookedTermios(consoleTermios);
	for (int i = 0; i < MAXFD; i++) {
		fds[i].used = false;
		fds[i].isConsole = false;
		fds[i].offset = 0;
		fds[i].size = 0;
		fds[i].flags = 0;
		fds[i].cloexec = false;
		fds[i].pipe = 0;
		fds[i].pipeWrite = false;
		fds[i].sock = 0;
	}
	// fd 0,1,2 = stdin/stdout/stderr -> console.
	for (int i = 0; i < 3; i++) {
		fds[i].used = true;
		fds[i].isConsole = true;
	}
}

// fork(2): a child inherits a copy of the fd table. Copy every descriptor (String paths
// deep-copy via String's assignment) and, for each open pipe end, bump the matching
// refcount — parent and child share the same Pipe object, so both ends must be counted.
Syscalls::Syscalls(const Syscalls& o) {
	vfs = o.vfs;
	consoleWrite = o.consoleWrite;
	consoleTermios = o.consoleTermios;   // inherit the parent's terminal settings
	m_cwd = o.m_cwd;                     // child inherits the parent's working directory
	m_umask = o.m_umask;                 // and the file-creation mask
	m_uid = o.m_uid; m_gid = o.m_gid;    // and the credentials
	m_euid = o.m_euid; m_egid = o.m_egid;
	exited = o.exited;
	exitCode = o.exitCode;
	for (int i = 0; i < MAXFD; i++) {
		fds[i] = o.fds[i];
		if (fds[i].used && fds[i].pipe) {
			if (fds[i].pipeWrite) fds[i].pipe->addWriter();
			else fds[i].pipe->addReader();
		}
		if (fds[i].used && fds[i].sock)      // fork shares the socket (refcount, like a pipe end)
			socketRef(fds[i].sock);
	}
}

// Process exit: close any open descriptors so pipe-end refcounts drop (and pipes free at
// zero) — this is what lets a reader see EOF once the last writing process is gone.
Syscalls::~Syscalls() {
	for (int i = 0; i < MAXFD; i++)
		if (fds[i].used && (fds[i].pipe || fds[i].sock))
			close(i);
}

// Close every open descriptor at process exit (procExit/procKill), so a pipe's last writer
// going away surfaces EOF to the reader immediately — not only when the parent reaps the
// zombie. Idempotent: close() skips already-closed fds, so the destructor at reap is a no-op.
void Syscalls::closeAll() {
	for (int i = 0; i < MAXFD; i++)
		if (fds[i].used)
			close(i);
}

int Syscalls::open(String path, int flags) {
	path = resolvePath(path);   // relative -> against the process cwd (stored absolute on the fd)
	FileStat st;
	bool exists = vfs->stat(path, st) >= 0;
	bool preExisted = exists;
	// O_CREAT (and O_TRUNC) ask the filesystem to make-or-truncate the file. On a
	// read-only fs create() returns -EROFS; if the file already exists we ignore that
	// and open it read-only, otherwise the open fails.
	if (flags & (O_CREAT | O_TRUNC)) {
		int cr = vfs->create(path, 0644);
		if (cr == 0) {
			st.size = 0;
			exists = true;
		} else if (!exists) {
			return cr < 0 ? cr : -ENOENT;
		}
	}
	if (!exists)
		return -ENOENT;
	// Permission check against the open mode (read/write intent) for a pre-existing file.
	if (preExisted) {
		int want = (flags & 3) == 1 ? 2 : (flags & 3) == 2 ? 6 : 4;   // WRONLY=w, RDWR=rw, else r
		int pc = permCheck(st, want);
		if (pc < 0)
			return pc;
	}
	for (int fd = 3; fd < MAXFD; fd++) {
		if (!fds[fd].used) {
			fds[fd].used = true;
			fds[fd].isConsole = false;
			fds[fd].pipe = 0;
			fds[fd].pipeWrite = false;
			fds[fd].path = path;
			fds[fd].offset = 0;
			fds[fd].size = st.size;
			fds[fd].flags = (unsigned) flags;
			fds[fd].cloexec = (flags & O_CLOEXEC) != 0;   // O_CLOEXEC -> close on execve
			return fd;
		}
	}
	return -EBADF;   // out of descriptors
}

int Syscalls::close(int fd, bool* freedShared) {
	if (freedShared) *freedShared = false;
	if (!valid(fd))
		return -EBADF;
	if (fds[fd].pipe) {                       // drop this end; free the pipe when both gone
		Pipe* p = fds[fd].pipe;
		if (fds[fd].pipeWrite) p->dropWriter(); else p->dropReader();
		if (p->readers() == 0 && p->writers() == 0) {
			delete p;                         // the pipe's WaitQueue dies with it
			if (freedShared) *freedShared = true;
		}
		fds[fd].pipe = 0;
		fds[fd].pipeWrite = false;
	}
	if (fds[fd].sock) {                       // drop this socket reference (frees at the last close)
		socketClose(fds[fd].sock);
		fds[fd].sock = 0;
	}
	fds[fd].used = false;
	fds[fd].isConsole = false;
	fds[fd].cloexec = false;
	return 0;
}

// Lowest free descriptor >= from (POSIX), or -EMFILE. `from` defaults to 0, so a closed
// 0/1/2 is reusable (dup2); F_DUPFD passes a floor.
int Syscalls::allocFd(int from) {
	if (from < 0) from = 0;
	for (int fd = from; fd < MAXFD; fd++)
		if (!fds[fd].used)
			return fd;
	return -EMFILE;
}

// Close every descriptor marked FD_CLOEXEC. Called by execve so the new image does not
// inherit the shell's private fds (history files, pipe ends, etc.).
void Syscalls::closeCloexec() {
	for (int fd = 0; fd < MAXFD; fd++)
		if (fds[fd].used && fds[fd].cloexec)
			close(fd);
}

// Make dst an independent descriptor aliasing src's underlying object (dup/dup2). A pipe
// end bumps the matching refcount so both descriptors keep the pipe alive.
void Syscalls::shareInto(int dst, int src) {
	fds[dst].used = true;
	fds[dst].isConsole = fds[src].isConsole;
	fds[dst].path = fds[src].path;
	fds[dst].offset = fds[src].offset;
	fds[dst].size = fds[src].size;
	fds[dst].flags = fds[src].flags;
	fds[dst].cloexec = false;   // a dup'd fd never inherits FD_CLOEXEC (POSIX); F_DUPFD_CLOEXEC sets it after
	fds[dst].pipe = fds[src].pipe;
	fds[dst].pipeWrite = fds[src].pipeWrite;
	fds[dst].sock = fds[src].sock;
	if (fds[dst].pipe) {
		if (fds[dst].pipeWrite) fds[dst].pipe->addWriter();
		else fds[dst].pipe->addReader();
	}
	if (fds[dst].sock)               // dup shares the socket (refcount)
		socketRef(fds[dst].sock);
}

int Syscalls::pipe(int out[2]) {
	Pipe* p = new Pipe();
	p->addReader();
	p->addWriter();
	int r = allocFd();
	if (r < 0) { delete p; return r; }
	fds[r].used = true; fds[r].isConsole = false; fds[r].path = String();
	fds[r].offset = 0; fds[r].size = 0; fds[r].flags = 0; fds[r].cloexec = false;
	fds[r].pipe = p; fds[r].pipeWrite = false;
	int w = allocFd();
	if (w < 0) { close(r); return w; }
	fds[w].used = true; fds[w].isConsole = false; fds[w].path = String();
	fds[w].offset = 0; fds[w].size = 0; fds[w].flags = 0; fds[w].cloexec = false;
	fds[w].pipe = p; fds[w].pipeWrite = true;
	out[0] = r;
	out[1] = w;
	return 0;
}

int Syscalls::dup(int fd) {
	if (!valid(fd))
		return -EBADF;
	int n = allocFd();
	if (n < 0)
		return n;
	shareInto(n, fd);
	return n;
}

int Syscalls::dup2(int oldfd, int newfd) {
	if (!valid(oldfd))
		return -EBADF;
	if (newfd < 0 || newfd >= MAXFD)
		return -EBADF;
	if (oldfd == newfd)
		return newfd;
	if (fds[newfd].used)
		close(newfd);
	shareInto(newfd, oldfd);
	return newfd;
}

bool Syscalls::fdReadable(int fd) {
	if (!valid(fd)) return false;
	if (fds[fd].pipe) return fds[fd].pipe->readable() || fds[fd].pipe->atEof();
	return true;   // files/devices: assume ready (console handled in the dispatch)
}

WaitQueue* Syscalls::fdWaitQueue(int fd) {
	if (!valid(fd)) return 0;
	if (fds[fd].sock) return &fds[fd].sock->rxWait;   // blocked recv/accept/connect park here
	if (fds[fd].pipe) return fds[fd].pipe->waitQueue();
	if (fds[fd].isConsole) return 0;          // console blocks inside arch::inputRead, not here
	return vfs->waitQueueAt(fds[fd].path);    // a char device (pty) exposes its queue; else 0
}

bool Syscalls::fdWritable(int fd) {
	if (!valid(fd)) return false;
	if (fds[fd].pipe) return fds[fd].pipe->writable() || fds[fd].pipe->readers() == 0;
	return true;
}

// Non-blocking scan: fill revents, return number of ready fds. Console POLLIN readiness is
// resolved in the dispatch (it knows the input layer); here a console reports POLLOUT ready.
int Syscalls::pollScan(PollFd* pfds, int nfds) {
	int ready = 0;
	for (int i = 0; i < nfds; i++) {
		short ev = pfds[i].events;
		short re = 0;
		int fd = pfds[i].fd;
		if (fd < 0) {
			pfds[i].revents = 0;       // negative fd: ignored, never ready
			continue;
		}
		if (!valid(fd)) {
			re = POLLNVAL;
		} else if (fds[fd].sock) {
			int se = socketPoll(fds[fd].sock);   // POLLIN/POLLOUT/POLLERR (CharDevice.h values match)
			re |= (short) (se & ev);
			if (se & POLLERR) re |= POLLERR;
		} else if (fds[fd].pipe) {
			Pipe* p = fds[fd].pipe;
			if (!fds[fd].pipeWrite) {
				if ((ev & POLLIN) && (p->readable() || p->atEof())) re |= POLLIN;
				if (p->atEof()) re |= POLLHUP;
			} else {
				if ((ev & POLLOUT) && p->writable()) re |= POLLOUT;
				if (p->readers() == 0) re |= POLLERR;
			}
		} else if (fds[fd].isConsole) {
			re |= ev & (POLLIN | POLLOUT);     // console: treat as ready (refined later)
		} else {
			// File or device node: ask the VFS (files report ready; pty/keyboard report
			// their real buffer state via CharDevice::pollReady).
			re |= vfs->pollReady(fds[fd].path, ev);
		}
		pfds[i].revents = re;
		if (re)
			ready++;
	}
	return ready;
}

int Syscalls::read(int fd, void* buf, unsigned n) {
	if (!valid(fd))
		return -EBADF;
	if (fds[fd].sock) {                       // recv on a socket (TCP byte stream / UDP datagram)
		Socket* s = fds[fd].sock;
		if (s->domain == AF_PACKET)           // AF_PACKET read() must strip L2 (cooked) — busybox
			return packetRecv(s, buf, n, 0, 0, 0, 0, 0);   // udhcpc reads the OFFER via read(), not recvfrom
		return socketRecvFrom(s, buf, n, 0, 0, 0);
	}
	if (fds[fd].pipe) {                       // read end of a pipe
		if (fds[fd].pipeWrite)
			return -EBADF;                    // can't read the write end
		int r = fds[fd].pipe->read(buf, n);
		if (r == 0 && !fds[fd].pipe->atEof())
			return -EAGAIN;                   // empty but writers remain -> would block
		return r;                             // r>0 = data; r==0 = EOF (all writers closed)
	}
	if (fds[fd].isConsole)
		// cooked line or raw bytes; 0 = EOF. O_NONBLOCK -> -EAGAIN instead of blocking.
		return arch::inputRead((char*) buf, n, (fds[fd].flags & O_NONBLOCK) != 0);
	int r = vfs->read(fds[fd].path, n, fds[fd].offset, buf);
	if (r < 0)
		return r;   // propagate the error (e.g. -EAGAIN would-block, -EIO device error)
	// r == 0 is true EOF; r > 0 is data. (Filesystems signal EOF as 0, not a negative.)
	fds[fd].offset += (unsigned) r;
	return r;
}

int Syscalls::write(int fd, const void* buf, unsigned n) {
	if (!valid(fd))
		return -EBADF;
	if (fds[fd].sock)                         // send on a socket (must be connected, like write(2))
		return socketSendTo(fds[fd].sock, buf, n, 0, 0);
	if (fds[fd].pipe) {                       // write end of a pipe
		if (!fds[fd].pipeWrite)
			return -EBADF;
		if (fds[fd].pipe->readers() == 0)
			return -EPIPE;                    // no reader left (would raise SIGPIPE on Linux)
		int w = fds[fd].pipe->write(buf, n);
		if (w == 0)
			return -EAGAIN;                   // full -> would block
		return w;
	}
	if (fds[fd].isConsole)
		return consoleWrite((const char*) buf, n);
	if (fds[fd].flags & O_APPEND)             // O_APPEND: each write lands at end-of-file
		fds[fd].offset = fds[fd].size;
	// Route to the VFS: ordinary files return -EROFS (the default), but a device node
	// (e.g. /dev/fb0) accepts the write.
	int r = vfs->write(fds[fd].path, n, fds[fd].offset, buf);
	if (r > 0) {
		fds[fd].offset += (unsigned) r;
		if (fds[fd].offset > fds[fd].size)   // track growth so SEEK_END/fstat stay correct
			fds[fd].size = fds[fd].offset;
	}
	return r;
}

int Syscalls::lseek(int fd, int off, int whence) {
	if (!valid(fd))
		return -EBADF;
	int base;
	if (whence == SEEK_SET)
		base = 0;
	else if (whence == SEEK_CUR)
		base = (int) fds[fd].offset;
	else if (whence == SEEK_END)
		base = (int) fds[fd].size;
	else
		return -EINVAL;
	int pos = base + off;
	if (pos < 0)
		return -EINVAL;
	fds[fd].offset = (unsigned) pos;
	return pos;
}

// Fill a LinuxStat from a VFS FileStat, falling back to sane defaults for fields
// a filesystem may not record (mode/nlink). Shared by stat() and fstat().
static void fillStat(LinuxStat* out, const FileStat& st) {
	out->st_mode = st.mode ? st.mode
	             : ((st.type == NODE_DIR) ? 0x41EDu : 0x81A4u);   // 0755 dir / 0644 file
	out->st_size = st.size;
	out->st_nlink = st.nlink ? st.nlink : 1;
	out->st_uid = st.uid;
	out->st_gid = st.gid;
	out->st_mtime = st.mtime;
	out->st_ino = 1;
}

int Syscalls::stat(String path, LinuxStat* out) {
	path = resolvePath(path);
	FileStat st;
	if (vfs->stat(path, st) < 0)
		return -ENOENT;
	fillStat(out, st);
	return 0;
}

int Syscalls::lstat(String path, LinuxStat* out) {
	path = resolvePath(path);
	FileStat st;
	if (vfs->lstat(path, st) < 0)
		return -ENOENT;
	fillStat(out, st);   // ext sets st.mode incl. the S_IFLNK format bits for a symlink
	return 0;
}

int Syscalls::readlink(String path, char* buf, unsigned size) {
	return vfs->readlink(resolvePath(path), buf, size);
}

int Syscalls::fstat(int fd, LinuxStat* out) {
	if (!valid(fd))
		return -EBADF;
	if (fds[fd].isConsole) {
		out->st_mode = 0x2000;   // S_IFCHR
		out->st_size = 0;
		out->st_nlink = 1;
		out->st_uid = 0;
		out->st_gid = 0;
		out->st_mtime = 0;
		out->st_ino = 0;
		return 0;
	}
	FileStat st;
	if (vfs->stat(fds[fd].path, st) < 0)
		return -EBADF;
	fillStat(out, st);
	return 0;
}

// Linux dirent64: u64 d_ino; s64 d_off; u16 d_reclen; u8 d_type; char d_name[];
// The name starts at byte 19; reclen is 8-byte aligned.
//
// getdents64 is a CURSOR: each call returns the next batch of entries that fit in `buf` and
// returns 0 only at true end-of-directory. We reuse the fd's `offset` as the cursor (the
// number of entries already handed out), so a caller looping until 0 sees every entry even
// when the buffer holds only one record at a time. If the buffer can't hold even one record,
// we return -EINVAL (as Linux does) rather than 0 — a 0 there would be a silent "no more
// entries" lie that drops the rest of the directory.
int Syscalls::getdents64(int fd, void* buf, unsigned n) {
	if (!valid(fd) || fds[fd].isConsole)
		return -EBADF;
	List<DirEntry> entries;
	if (vfs->readdir(fds[fd].path, entries) < 0)
		return -EBADF;
	char* out = (char*) buf;
	unsigned pos = 0;
	int i = (int) fds[fd].offset;          // resume where the previous call stopped
	for (; i < entries.getCount(); i++) {
		const char* name = entries[i].name;
		unsigned namelen = (unsigned) strlen(name);
		unsigned reclen = (19 + namelen + 1 + 7) & ~7u;
		if (pos + reclen > n) {
			if (pos == 0)
				return -EINVAL;            // buffer too small for a single record
			break;                         // no room for this one; resume it next call
		}
		char* rec = out + pos;
		memset(rec, 0, reclen);
		*(unsigned*) (rec + 0) = (unsigned) (i + 1);          // d_ino (low 32)
		*(unsigned short*) (rec + 16) = (unsigned short) reclen; // d_reclen
		rec[18] = (entries[i].type == NODE_DIR) ? 4 : 8;      // d_type (DT_DIR/DT_REG)
		for (unsigned k = 0; k < namelen; k++)
			rec[19 + k] = name[k];
		pos += reclen;
	}
	fds[fd].offset = (unsigned) i;         // advance cursor; next call resumes (or hits EOF)
	return (int) pos;
}

int Syscalls::ioctl(int fd, unsigned cmd, void* arg) {
	if (!valid(fd))
		return -EBADF;
	if (fds[fd].isConsole) {
		// The console IS a terminal. We keep a real Termios for it: TCGETS returns the
		// current settings and TCSETS replaces them (so tcgetattr/tcsetattr round-trip and
		// a program can actually flip canonical/echo). The dispatch reads consoleRaw()
		// after a TCSETS to drive the line discipline. Winsize/pgrp stay unsupported here.
		switch (cmd) {
		case IOCTL_TCGETS:
			if (!arg) return -EINVAL;
			*(Termios*) arg = consoleTermios;
			return 0;
		case IOCTL_TCSETS:
		case IOCTL_TCSETSW:
		case IOCTL_TCSETSF:
			if (!arg) return -EINVAL;
			consoleTermios = *(const Termios*) arg;
			return 0;
		default:
			return -EINVAL;
		}
	}
	return vfs->ioctl(fds[fd].path, cmd, arg);
}

int Syscalls::ttyPgrp(int fd) {
	if (!valid(fd) || fds[fd].pipe || fds[fd].isConsole)
		return -1;               // not a device fd -> not a tty
	int pgrp = -1;
	if (vfs->ioctl(fds[fd].path, 0x540Fu /* TIOCGPGRP */, &pgrp) < 0)
		return -1;               // the device does not implement TIOCGPGRP -> not a tty
	return pgrp;
}

int Syscalls::unlink(String path) {
	return vfs->unlink(resolvePath(path));
}

int Syscalls::mkdir(String path, int mode) {
	return vfs->mkdir(resolvePath(path), (unsigned) mode);
}

int Syscalls::rmdir(String path) {
	return vfs->rmdir(resolvePath(path));
}

int Syscalls::rename(String oldpath, String newpath) {
	return vfs->rename(resolvePath(oldpath), resolvePath(newpath));
}

int Syscalls::link(String oldpath, String newpath) {
	return vfs->link(resolvePath(oldpath), resolvePath(newpath));
}

int Syscalls::symlink(String target, String path) {
	// The target is the link's literal content (may be relative); only the new path is resolved.
	return vfs->symlink(target, resolvePath(path));
}

// ---- Phase 4: metadata, statfs, *at family ------------------------------------------------
#define AT_FDCWD            (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR        0x200

// Fill a Linux i386 struct statfs (64 bytes) from our StatFs.
static void fillStatfs(void* buf, const StatFs& s) {
	unsigned* f = (unsigned*) buf;
	for (int i = 0; i < 16; i++) f[i] = 0;
	f[0] = 0xEF53;            // f_type = EXT2_SUPER_MAGIC
	f[1] = s.blockSize;       // f_bsize
	f[2] = s.totalBlocks;     // f_blocks
	f[3] = s.freeBlocks;      // f_bfree
	f[4] = s.freeBlocks;      // f_bavail
	f[5] = s.totalInodes;     // f_files
	f[6] = s.freeInodes;      // f_ffree
	f[9] = s.nameMax;         // f_namelen (offset 36)
	f[10] = s.blockSize;      // f_frsize
}

int Syscalls::chmod(String path, int mode) {
	return vfs->chmod(resolvePath(path), (unsigned) mode & 0xFFFu);
}
int Syscalls::fchmod(int fd, int mode) {
	if (!valid(fd) || fds[fd].isConsole) return -9;          // -EBADF
	return vfs->chmod(fds[fd].path, (unsigned) mode & 0xFFFu);
}
int Syscalls::chown(String path, int uid, int gid) {
	return vfs->chown(resolvePath(path), (unsigned) uid, (unsigned) gid);
}
int Syscalls::lchown(String path, int uid, int gid) {
	return vfs->lchown(resolvePath(path), (unsigned) uid, (unsigned) gid);   // no-follow
}
int Syscalls::fchown(int fd, int uid, int gid) {
	if (!valid(fd) || fds[fd].isConsole) return -9;
	return vfs->chown(fds[fd].path, (unsigned) uid, (unsigned) gid);
}
int Syscalls::truncate(String path, unsigned length) {
	return vfs->truncate(resolvePath(path), length);
}
int Syscalls::ftruncate(int fd, unsigned length) {
	if (!valid(fd) || fds[fd].isConsole) return -9;
	int r = vfs->truncate(fds[fd].path, length);
	if (r == 0) fds[fd].size = length;
	return r;
}
int Syscalls::utimes(String path, unsigned atime, unsigned mtime) {
	return vfs->utimes(resolvePath(path), atime, mtime);
}
int Syscalls::access(String path, int mode) {
	FileStat st;
	if (vfs->stat(resolvePath(path), st) < 0) return -2;     // -ENOENT
	// We run as uid 0: read/write are always permitted; execute (X_OK=1) needs at least one
	// of the file's execute bits set (matches Linux root semantics).
	if ((mode & 1) && (st.mode & 0111) == 0) return -13;     // -EACCES
	return 0;
}
int Syscalls::statfs(String path, void* buf) {
	StatFs s;
	int r = vfs->statfs(resolvePath(path), s);
	if (r < 0) return r;
	fillStatfs(buf, s);
	return 0;
}
int Syscalls::fstatfs(int fd, void* buf) {
	if (!valid(fd)) return -9;
	if (fds[fd].isConsole) return -22;                       // -EINVAL
	StatFs s;
	int r = vfs->statfs(fds[fd].path, s);
	if (r < 0) return r;
	fillStatfs(buf, s);
	return 0;
}
int Syscalls::fsync(int fd) {
	if (!valid(fd)) return -9;
	return 0;                                                // our writes flush synchronously
}
int Syscalls::fchdir(int fd) {
	if (!valid(fd) || fds[fd].isConsole) return -9;
	return chdir(fds[fd].path);
}
int Syscalls::umask(int mask) {
	int old = (int) m_umask;
	m_umask = (unsigned) mask & 0777u;
	return old;
}
int Syscalls::creat(String path, int mode) {
	(void) mode;
	return open(resolvePath(path), O_CREAT | O_TRUNC | 1 /*O_WRONLY*/);
}

String Syscalls::resolveAt(int dirfd, String path, bool* badfd) {
	if (badfd) *badfd = false;
	const char* p = (char*) path;
	if ((p && p[0] == '/') || dirfd == AT_FDCWD)
		return resolvePath(path);
	if (!valid(dirfd) || fds[dirfd].isConsole) {
		if (badfd) *badfd = true;
		return resolvePath(path);
	}
	char buf[512];
	int k = 0;
	const char* d = (char*) fds[dirfd].path;
	while (d[k] && k < 400) { buf[k] = d[k]; k++; }
	if (k == 0 || buf[k - 1] != '/') buf[k++] = '/';
	int j = 0;
	while (p[j] && k < 510) buf[k++] = p[j++];
	buf[k] = 0;
	return resolvePath(String(buf));
}

int Syscalls::openat(int dirfd, String path, int flags) {
	bool bad; String p = resolveAt(dirfd, path, &bad);
	if (bad) return -9;
	return open(p, flags);
}
int Syscalls::mkdirat(int dirfd, String path, int mode) {
	bool bad; String p = resolveAt(dirfd, path, &bad);
	if (bad) return -9;
	return vfs->mkdir(p, (unsigned) mode & ~m_umask);
}
int Syscalls::unlinkat(int dirfd, String path, int flags) {
	bool bad; String p = resolveAt(dirfd, path, &bad);
	if (bad) return -9;
	return (flags & AT_REMOVEDIR) ? vfs->rmdir(p) : vfs->unlink(p);
}
int Syscalls::renameat(int oldfd, String oldpath, int newfd, String newpath) {
	bool b1, b2; String a = resolveAt(oldfd, oldpath, &b1); String b = resolveAt(newfd, newpath, &b2);
	if (b1 || b2) return -9;
	return vfs->rename(a, b);
}
int Syscalls::linkat(int oldfd, String oldpath, int newfd, String newpath, int flags) {
	(void) flags;
	bool b1, b2; String a = resolveAt(oldfd, oldpath, &b1); String b = resolveAt(newfd, newpath, &b2);
	if (b1 || b2) return -9;
	return vfs->link(a, b);
}
int Syscalls::symlinkat(String target, int newfd, String path) {
	bool bad; String p = resolveAt(newfd, path, &bad);
	if (bad) return -9;
	return vfs->symlink(target, p);
}
int Syscalls::readlinkat(int dirfd, String path, char* buf, unsigned size) {
	bool bad; String p = resolveAt(dirfd, path, &bad);
	if (bad) return -9;
	return readlink(p, buf, size);
}
int Syscalls::fchmodat(int dirfd, String path, int mode, int flags) {
	(void) flags;
	bool bad; String p = resolveAt(dirfd, path, &bad);
	if (bad) return -9;
	return vfs->chmod(p, (unsigned) mode & 0xFFFu);
}
int Syscalls::fchownat(int dirfd, String path, int uid, int gid, int flags) {
	(void) flags;
	bool bad; String p = resolveAt(dirfd, path, &bad);
	if (bad) return -9;
	return vfs->chown(p, (unsigned) uid, (unsigned) gid);
}
int Syscalls::fstatat(int dirfd, String path, LinuxStat* out, int flags) {
	bool bad; String p = resolveAt(dirfd, path, &bad);
	if (bad) return -9;
	return (flags & AT_SYMLINK_NOFOLLOW) ? lstat(p, out) : stat(p, out);
}

static unsigned rd32le(const void* p, unsigned off) {
	const unsigned char* b = (const unsigned char*) p + off;
	return (unsigned) b[0] | ((unsigned) b[1] << 8) | ((unsigned) b[2] << 16) | ((unsigned) b[3] << 24);
}

unsigned Syscalls::currentTime() {
	return wallClockSeconds();
}

int Syscalls::getuid()  { return (int) m_uid; }
int Syscalls::geteuid() { return (int) m_euid; }
int Syscalls::getgid()  { return (int) m_gid; }
int Syscalls::getegid() { return (int) m_egid; }
// Root may set any id; a non-root process may only switch among ids it already holds (here just
// its own), so a real uid change is root-only — enough to model privilege drop.
int Syscalls::setuid(int uid) {
	if (m_euid != 0 && (unsigned) uid != m_uid && (unsigned) uid != m_euid) return -1;   // -EPERM
	m_uid = m_euid = (unsigned) uid;
	return 0;
}
int Syscalls::setgid(int gid) {
	if (m_euid != 0 && (unsigned) gid != m_gid && (unsigned) gid != m_egid) return -1;
	m_gid = m_egid = (unsigned) gid;
	return 0;
}

// POSIX permission check for `want` (r=4/w=2/x=1) against the file's mode + owner, using the
// process's effective ids. Root (euid 0) gets r/w unconditionally and x if any execute bit is set.
int Syscalls::permCheck(const FileStat& st, int want) {
	if (want == 0) return 0;
	unsigned mode = st.mode;
	if (m_euid == 0) {
		if ((want & 1) && (mode & 0111) == 0) return -13;     // -EACCES (root still needs an x bit)
		return 0;
	}
	unsigned bits;
	if (st.uid == m_euid)        bits = (mode >> 6) & 7;       // owner
	else if (st.gid == m_egid)   bits = (mode >> 3) & 7;       // group
	else                         bits = mode & 7;             // other
	return ((bits & (unsigned) want) == (unsigned) want) ? 0 : -13;
}

int Syscalls::utime(String path, const void* times) {
	String p = resolvePath(path);
	unsigned at, mt;
	if (!times) { at = mt = currentTime(); }       // NULL -> now
	else { at = rd32le(times, 0); mt = rd32le(times, 8); }   // struct utimbuf {time_t actime, modtime}
	return vfs->utimes(p, at, mt);
}

int Syscalls::utimensat(int dirfd, String path, const void* times, int flags) {
	bool bad; String p = resolveAt(dirfd, path, &bad);
	if (bad) return -9;
	const unsigned UTIME_NOW = 0x3fffffff, UTIME_OMIT = 0x3ffffffe;
	unsigned at, mt;
	bool setA = true, setM = true;
	if (!times) { at = mt = currentTime(); }
	else {
		// struct timespec[2]: each {long long tv_sec @0; long tv_nsec @8} = 12 bytes.
		unsigned aSec = rd32le(times, 0), aNs = rd32le(times, 8);
		unsigned mSec = rd32le(times, 12), mNs = rd32le(times, 20);
		if (aNs == UTIME_NOW) at = currentTime(); else if (aNs == UTIME_OMIT) setA = false; else at = aSec;
		if (mNs == UTIME_NOW) mt = currentTime(); else if (mNs == UTIME_OMIT) setM = false; else mt = mSec;
	}
	if (!setA || !setM) {                          // keep the omitted field's current value
		FileStat st;
		unsigned cur = (vfs->stat(p, st) == 0) ? st.mtime : 0;
		if (!setA) at = cur;
		if (!setM) mt = cur;
	}
	(void) flags;
	return vfs->utimes(p, at, mt);
}

int Syscalls::faccessat(int dirfd, String path, int mode, int flags) {
	(void) flags;
	bool bad; String p = resolveAt(dirfd, path, &bad);
	if (bad) return -9;
	FileStat st;
	if (vfs->stat(p, st) < 0) return -2;
	if ((mode & 1) && (st.mode & 0111) == 0) return -13;
	return 0;
}

int Syscalls::renameat2(int oldfd, String oldpath, int newfd, String newpath, int flags) {
	const int NOREPLACE = 1, EXCHANGE = 2;
	if (flags & ~(NOREPLACE | EXCHANGE)) return -22;     // -EINVAL: unknown flags
	bool b1, b2; String a = resolveAt(oldfd, oldpath, &b1); String b = resolveAt(newfd, newpath, &b2);
	if (b1 || b2) return -9;
	FileStat st;
	bool destExists = vfs->stat(b, st) == 0;
	if (flags & NOREPLACE) {
		if (destExists) return -17;                      // -EEXIST
		return vfs->rename(a, b);
	}
	if (flags & EXCHANGE) {
		if (!destExists) return -2;                      // both must exist to swap
		// Non-atomic swap via a temporary name (no fs-level atomic exchange yet).
		char tmp[520]; const char* bs = (char*) b; int k = 0;
		while (bs[k] && k < 511) { tmp[k] = bs[k]; k++; }
		const char* sfx = ".rnxchg"; for (int i = 0; sfx[i] && k < 519; i++) tmp[k++] = sfx[i];
		tmp[k] = 0;
		if (vfs->rename(b, String(tmp)) != 0) return -16;
		if (vfs->rename(a, b) != 0) { vfs->rename(String(tmp), b); return -5; }
		if (vfs->rename(String(tmp), a) != 0) return -5;
		return 0;
	}
	return vfs->rename(a, b);
}

// Normalise any path to a clean absolute one: relative paths join onto the cwd, then "."
// and empty components drop and ".." pops the previous component ("foo/../bar" -> "/bar").
// Pure string work over char buffers (host-testable). This is what makes a child process
// inherit its parent's directory — the cwd lives in the kernel, not faked per-process.
String Syscalls::resolvePath(String path) {
	const char* in = (char*) path;
	char joined[512];
	if (in && in[0] == '/') {
		unsigned i = 0;
		while (in[i] && i < 511) { joined[i] = in[i]; i++; }
		joined[i] = 0;
	} else {
		const char* base = (char*) m_cwd;
		unsigned l = 0;
		while (base && base[l] && l < 511) { joined[l] = base[l]; l++; }
		if (l == 0) { joined[l++] = '/'; }
		if (joined[l - 1] != '/' && l < 511) joined[l++] = '/';
		for (unsigned k = 0; in && in[k] && l < 511; k++) joined[l++] = in[k];
		joined[l] = 0;
	}

	// Split on '/', dropping ""/"." and popping the previous segment on "..".
	const char* seg[64];
	int seglen[64], n = 0;
	const char* p = joined;
	while (*p) {
		while (*p == '/') p++;
		if (!*p) break;
		const char* s = p;
		while (*p && *p != '/') p++;
		int len = (int) (p - s);
		if (len == 1 && s[0] == '.') continue;
		if (len == 2 && s[0] == '.' && s[1] == '.') { if (n > 0) n--; continue; }
		if (n < 64) { seg[n] = s; seglen[n] = len; n++; }
	}

	char out[512];
	char* o = out;
	char* end = out + 510;
	if (n == 0) return String("/");
	for (int i = 0; i < n && o < end; i++) {
		*o++ = '/';
		for (int k = 0; k < seglen[i] && o < end; k++)
			*o++ = seg[i][k];
	}
	*o = 0;
	return String(out);
}

int Syscalls::chdir(String path) {
	String abs = resolvePath(path);
	FileStat st;
	if (vfs->stat(abs, st) < 0)
		return -ENOENT;
	if (st.type != NODE_DIR)
		return -ENOTDIR;
	m_cwd = abs;
	return 0;
}

int Syscalls::getcwd(char* buf, unsigned size) {
	const char* c = (char*) m_cwd;
	unsigned len = 0;
	while (c && c[len]) len++;
	if (size == 0 || len + 1 > size)
		return -ERANGE;
	for (unsigned i = 0; i <= len; i++)   // includes the NUL
		buf[i] = c[i];
	return (int) (len + 1);               // Linux getcwd returns the length incl. NUL
}

int Syscalls::fcntl(int fd, int cmd, int arg) {
	if (!valid(fd))
		return -EBADF;
	switch (cmd) {
	case F_GETFL:
		return (int) fds[fd].flags;
	case F_SETFL:
		// F_SETFL changes only the file STATUS flags, and we genuinely implement just one
		// (O_NONBLOCK). Toggle that bit and leave the rest of the descriptor's flags as they
		// were at open — don't store flags we don't honor (that would let F_GETFL report a
		// behaviour we never apply). Unsupported status bits are ignored, as on Linux.
		fds[fd].flags = (fds[fd].flags & ~(unsigned) O_NONBLOCK)
		              | ((unsigned) arg & (unsigned) O_NONBLOCK);
		return 0;
	case F_GETFD:
		return fds[fd].cloexec ? FD_CLOEXEC : 0;
	case F_SETFD:
		fds[fd].cloexec = (arg & FD_CLOEXEC) != 0;
		return 0;
	case F_DUPFD:
	case F_DUPFD_CLOEXEC: {
		int n = allocFd(arg);            // lowest free descriptor >= arg
		if (n < 0)
			return n;
		shareInto(n, fd);                // shareInto clears cloexec...
		fds[n].cloexec = (cmd == F_DUPFD_CLOEXEC);   // ...F_DUPFD_CLOEXEC re-sets it
		return n;
	}
	}
	return -EINVAL;
}

int Syscalls::mmapInfo(int fd, unsigned* physOut, unsigned* lenOut) {
	if (!valid(fd) || fds[fd].isConsole)
		return -EBADF;
	return vfs->mmapInfo(fds[fd].path, physOut, lenOut);
}

void Syscalls::exit(int code) {
	exited = true;
	exitCode = code;
}

int Syscalls::clockGettime(int clkId, unsigned ticks, unsigned epochBaseSec, KTimespec* out) {
	// ticks are milliseconds since boot (1000 Hz). BOTH the seconds AND the sub-second nanos
	// derive from this single tick value, so the timestamp is monotonic. CLOCK_REALTIME (0)
	// just offsets by `epochBaseSec` — the RTC sampled ONCE at boot — to land on wall-clock time.
	// (Reading the live RTC seconds here instead would desync from the tick sub-second at every
	// RTC second-boundary, making realtime non-monotonic — which makes ping RTTs go negative.)
	// Every other clock id is monotonic: seconds since boot. (CLOCK_REALTIME == 0, MONOTONIC == 1.)
	if (!out)
		return -EINVAL;
	unsigned long long sec = ticks / 1000u;
	if (clkId == 0)
		sec += epochBaseSec;      // wall clock = boot epoch (RTC@boot) + monotonic uptime
	out->tv_sec = (long long) sec;
	out->tv_nsec = (int) ((ticks % 1000u) * 1000000u);
	return 0;
}

unsigned Syscalls::nanosleepMs(const KTimespec* req) {
	if (!req)
		return 0;
	long long sec = req->tv_sec < 0 ? 0 : req->tv_sec;
	int nsec = req->tv_nsec < 0 ? 0 : req->tv_nsec;
	// Round the sub-millisecond remainder up: a request for <1 ms still sleeps a tick.
	unsigned ms = (unsigned) sec * 1000u + (unsigned) ((nsec + 999999) / 1000000);
	return ms;
}

// ============================================================================
// Sockets (FAZA 9). The socket lives in the fd table; these marshal the Linux i686 sockaddr_in
// ABI (family LE, port + addr network order) and forward to the MI socket/UDP/RAW/TCP core.
// ============================================================================
namespace {
const int SA_AF_INET = 2;
// Parse a sockaddr_in -> host-order ip/port. Returns the family, or -1 if too short.
int parseSockaddr(const void* sa, unsigned salen, uint32_t* ip, uint16_t* port) {
	if (!sa || salen < 8) return -1;
	const unsigned char* p = (const unsigned char*) sa;
	int fam = p[0] | (p[1] << 8);
	if (port) *port = rd16be(p + 2);
	if (ip)   *ip   = rd32be(p + 4);
	return fam;
}
// Write a sockaddr_in (16 bytes) into sa, capped to *salen, and report the full size back.
void writeSockaddr(void* sa, unsigned* salen, uint32_t ip, uint16_t port) {
	if (!sa) { if (salen) *salen = 16; return; }
	unsigned char tmp[16];
	memset(tmp, 0, sizeof(tmp));
	tmp[0] = SA_AF_INET; tmp[1] = 0;
	wr16be(tmp + 2, port);
	wr32be(tmp + 4, ip);
	unsigned cap = salen ? *salen : 16;
	unsigned n = cap < 16 ? cap : 16;
	memcpy(sa, tmp, n);
	if (salen) *salen = 16;   // Linux reports the untruncated length
}

// struct sockaddr_ll (Linux i686, 20 bytes): family(2) protocol(2,net order) ifindex(4) hatype(2)
// pkttype(1) halen(1) addr[8]. protocol is kept network-order verbatim (the value the app passed).
const int SA_AF_PACKET = 17;
bool parseSockaddrLl(const void* sa, unsigned salen, int* ifx, uint16_t* proto, unsigned char* dmac) {
	if (!sa || salen < 8) return false;
	const unsigned char* p = (const unsigned char*) sa;
	if ((p[0] | (p[1] << 8)) != SA_AF_PACKET) return false;
	if (proto) *proto = (uint16_t) (p[2] | (p[3] << 8));
	if (ifx)   *ifx   = p[4] | (p[5] << 8) | (p[6] << 16) | (p[7] << 24);
	if (dmac && salen >= 18) memcpy(dmac, p + 12, 6);
	return true;
}
void writeSockaddrLl(void* sa, unsigned* salen, uint16_t proto, int ifx, int pkttype, const unsigned char* mac) {
	if (!sa) { if (salen) *salen = 20; return; }
	unsigned char t[20]; memset(t, 0, sizeof t);
	t[0] = SA_AF_PACKET;
	t[2] = proto & 0xff; t[3] = (proto >> 8) & 0xff;
	t[4] = ifx & 0xff; t[5] = (ifx >> 8) & 0xff; t[6] = (ifx >> 16) & 0xff; t[7] = (ifx >> 24) & 0xff;
	t[8] = 1;                       // hatype ARPHRD_ETHER
	t[10] = (unsigned char) pkttype;
	t[11] = 6;                      // halen
	if (mac) memcpy(t + 12, mac, 6);
	unsigned cap = salen ? *salen : 20; unsigned n = cap < 20 ? cap : 20;
	memcpy(sa, t, n);
	if (salen) *salen = 20;
}

// struct sockaddr_un (Linux): family(2) + sun_path[108]. salen - 2 = bytes of sun_path the caller
// supplied; for a named socket that includes the trailing NUL, for an abstract socket sun_path[0]
// is NUL (matched verbatim). The path bytes are the binding key, used as-is by net/Unix.cpp.
const int SA_AF_UNIX = 1;
bool parseSockaddrUn(const void* sa, unsigned salen, char* path, unsigned* pathLen) {
	if (!sa || salen < 2) return false;
	const unsigned char* p = (const unsigned char*) sa;
	if ((p[0] | (p[1] << 8)) != SA_AF_UNIX) return false;
	unsigned plen = salen - 2;
	if (plen > UNIX_PATH_MAX) plen = UNIX_PATH_MAX;
	for (unsigned i = 0; i < plen; i++) path[i] = (char) p[2 + i];
	*pathLen = plen;
	return true;
}
void writeSockaddrUn(void* sa, unsigned* salen, const char* path, unsigned pathLen) {
	unsigned total = 2 + pathLen;
	if (!sa) { if (salen) *salen = total; return; }
	if (pathLen > UNIX_PATH_MAX) pathLen = UNIX_PATH_MAX;
	unsigned char t[2 + UNIX_PATH_MAX]; memset(t, 0, sizeof t);
	t[0] = SA_AF_UNIX;
	for (unsigned i = 0; i < pathLen; i++) t[2 + i] = (unsigned char) path[i];
	unsigned cap = salen ? *salen : total; unsigned n = cap < total ? cap : total;
	memcpy(sa, t, n);
	if (salen) *salen = total;
}
}  // namespace

// Install socket `s` into a fresh fd, honoring SOCK_CLOEXEC(0x80000)/SOCK_NONBLOCK(0x800).

int Syscalls::sockSocket(int domain, int type, int protocol) {
	int base = type & 0xff;                       // strip SOCK_CLOEXEC/SOCK_NONBLOCK
	int err = 0;
	Socket* s = socketCreate(domain, base, protocol, &err);
	if (!s) return err;
	int fd = allocFd(0);
	if (fd < 0) { socketClose(s); return fd; }
	fds[fd].used = true; fds[fd].isConsole = false; fds[fd].pipe = 0; fds[fd].pipeWrite = false;
	fds[fd].sock = s; fds[fd].path = String(); fds[fd].offset = 0; fds[fd].size = 0;
	fds[fd].flags = 0; fds[fd].cloexec = (type & 0x80000) != 0;
	if (type & 0x800) fds[fd].flags |= O_NONBLOCK;
	return fd;
}

int Syscalls::sockBind(int fd, const void* sa, unsigned salen) {
	if (!isSocketFd(fd)) return -ENOTSOCK;
	Socket* s = fds[fd].sock;
	if (s->domain == AF_PACKET) {
		int ifx; uint16_t proto;
		if (!parseSockaddrLl(sa, salen, &ifx, &proto, 0)) return -EINVAL;
		return packetBind(s, ifx, proto);
	}
	if (s->domain == AF_UNIX) {
		char path[UNIX_PATH_MAX]; unsigned plen = 0;
		if (!parseSockaddrUn(sa, salen, path, &plen)) return -EINVAL;
		// Plant a visible S_IFSOCK node for a named (non-abstract) path on a writable fs. EEXIST
		// means the name is taken (the file persists until unlink, like Linux) -> EADDRINUSE. A
		// read-only / absent fs (e.g. /tmp not mounted in a host-test fixture) returns EROFS/ENOENT,
		// in which case the in-kernel registry alone is authoritative for the binding.
		if (plen > 0 && path[0] != 0 && vfs) {
			int mk = vfs->mknod(String(path), 0xC000 | 0777);
			if (mk == -17) return -EADDRINUSE;             // -EEXIST
		}
		return unixBind(s, path, plen);
	}
	uint32_t ip; uint16_t port;
	if (parseSockaddr(sa, salen, &ip, &port) != SA_AF_INET) return -EAFNOSUPPORT;
	return socketBind(s, ip, port);
}

int Syscalls::sockConnect(int fd, const void* sa, unsigned salen) {
	if (!isSocketFd(fd)) return -ENOTSOCK;
	Socket* us = fds[fd].sock;
	if (us->domain == AF_UNIX) {
		char path[UNIX_PATH_MAX]; unsigned plen = 0;
		if (!parseSockaddrUn(sa, salen, path, &plen)) return -EINVAL;
		return unixConnect(us, path, plen);    // local: connects immediately (no -EINPROGRESS)
	}
	uint32_t ip; uint16_t port;
	if (parseSockaddr(sa, salen, &ip, &port) != SA_AF_INET) return -EAFNOSUPPORT;
	int r = socketConnect(fds[fd].sock, ip, port);
	if (r < 0) return r;
	if (fds[fd].sock->type == SOCK_STREAM) return -EINPROGRESS;   // handshake started; dispatch blocks
	return 0;                                                     // UDP/RAW connect is immediate
}

int Syscalls::sockConnectResult(int fd) {
	if (!isSocketFd(fd)) return -EBADF;
	Socket* s = fds[fd].sock;
	if (s->type != SOCK_STREAM) return 0;
	int st = tcpState(s);
	if (st == TCP_ESTABLISHED) return 0;
	if (st == TCP_CLOSED) return s->soError ? -s->soError : -ECONNREFUSED;
	return -EINPROGRESS;
}

int Syscalls::sockListen(int fd, int backlog) {
	if (!isSocketFd(fd)) return -ENOTSOCK;
	Socket* s = fds[fd].sock;
	if (s->domain == AF_UNIX) return unixListen(s, backlog);
	if (s->type != SOCK_STREAM) return -EOPNOTSUPP;
	return tcpListen(s, backlog);
}

int Syscalls::sockAccept(int fd, void* sa, unsigned* salen) {
	if (!isSocketFd(fd)) return -ENOTSOCK;
	Socket* s = fds[fd].sock;
	int err = 0;
	Socket* ns = (s->domain == AF_UNIX) ? unixAccept(s, &err) : tcpAccept(s, &err);
	if (!ns) return err;                          // -EAGAIN if none waiting (dispatch blocks)
	int nfd = allocFd(0);
	if (nfd < 0) { socketClose(ns); return nfd; }
	fds[nfd].used = true; fds[nfd].isConsole = false; fds[nfd].pipe = 0; fds[nfd].pipeWrite = false;
	fds[nfd].sock = ns; fds[nfd].path = String(); fds[nfd].offset = 0; fds[nfd].size = 0;
	fds[nfd].flags = 0; fds[nfd].cloexec = false;
	if (sa) {
		if (s->domain == AF_UNIX) writeSockaddrUn(sa, salen, 0, 0);   // peer is unnamed (autobind)
		else { uint32_t ip; uint16_t port; socketGetPeerName(ns, &ip, &port); writeSockaddr(sa, salen, ip, port); }
	}
	return nfd;
}

// socketpair(2): only AF_UNIX is meaningful. Creates two pre-connected sockets and installs both
// into fresh descriptors (honoring SOCK_CLOEXEC/SOCK_NONBLOCK in `type`, like sockSocket).
int Syscalls::sockSocketpair(int domain, int type, int protocol, int sv[2]) {
	if (domain != AF_UNIX) return -EOPNOTSUPP;
	int baseType = type & ~(0x80000 | 0x800);     // strip SOCK_CLOEXEC / SOCK_NONBLOCK
	Socket* a = 0; Socket* b = 0;
	int rc = unixSocketpair(baseType, protocol, &a, &b);
	if (rc < 0) return rc;
	bool cloexec = (type & 0x80000) != 0;
	int nb = (type & 0x800) ? O_NONBLOCK : 0;
	int f0 = allocFd(0);
	if (f0 < 0) { socketClose(a); socketClose(b); return f0; }
	fds[f0].used = true; fds[f0].isConsole = false; fds[f0].pipe = 0; fds[f0].pipeWrite = false;
	fds[f0].sock = a; fds[f0].path = String(); fds[f0].offset = 0; fds[f0].size = 0;
	fds[f0].flags = nb; fds[f0].cloexec = cloexec;
	int f1 = allocFd(0);
	if (f1 < 0) { close(f0); socketClose(b); return f1; }
	fds[f1].used = true; fds[f1].isConsole = false; fds[f1].pipe = 0; fds[f1].pipeWrite = false;
	fds[f1].sock = b; fds[f1].path = String(); fds[f1].offset = 0; fds[f1].size = 0;
	fds[f1].flags = nb; fds[f1].cloexec = cloexec;
	sv[0] = f0; sv[1] = f1;
	return 0;
}

int Syscalls::sockGetsockopt(int fd, int level, int name, void* val, unsigned* len) {
	if (!isSocketFd(fd)) return -ENOTSOCK;
	return socketGetOpt(fds[fd].sock, level, name, val, len);
}
int Syscalls::sockSetsockopt(int fd, int level, int name, const void* val, unsigned len) {
	if (!isSocketFd(fd)) return -ENOTSOCK;
	return socketSetOpt(fds[fd].sock, level, name, val, len);
}
int Syscalls::sockGetsockname(int fd, void* sa, unsigned* salen) {
	if (!isSocketFd(fd)) return -ENOTSOCK;
	uint32_t ip; uint16_t port; socketGetSockName(fds[fd].sock, &ip, &port);
	writeSockaddr(sa, salen, ip, port);
	return 0;
}
int Syscalls::sockGetpeername(int fd, void* sa, unsigned* salen) {
	if (!isSocketFd(fd)) return -ENOTSOCK;
	uint32_t ip; uint16_t port;
	int r = socketGetPeerName(fds[fd].sock, &ip, &port);
	if (r < 0) return r;
	writeSockaddr(sa, salen, ip, port);
	return 0;
}

int Syscalls::sockSendto(int fd, const void* buf, unsigned len, int /*flags*/, const void* sa, unsigned salen) {
	if (!isSocketFd(fd)) return -ENOTSOCK;
	Socket* s = fds[fd].sock;
	if (s->domain == AF_PACKET) {
		int ifx = 0; uint16_t proto = (uint16_t) s->protocol; unsigned char dmac[6] = {0};
		bool have = sa && parseSockaddrLl(sa, salen, &ifx, &proto, dmac);
		return packetSend(s, buf, len, ifx, proto, have ? dmac : 0);
	}
	if (s->domain == AF_UNIX) {
		char path[UNIX_PATH_MAX]; unsigned plen = 0;
		bool have = sa && parseSockaddrUn(sa, salen, path, &plen);
		return unixSend(s, buf, len, have ? path : 0, have ? plen : 0);
	}
	uint32_t ip = 0; uint16_t port = 0;
	if (sa) { if (parseSockaddr(sa, salen, &ip, &port) != SA_AF_INET) return -EAFNOSUPPORT; }
	return socketSendTo(s, buf, len, ip, port);
}

int Syscalls::sockRecvfrom(int fd, void* buf, unsigned len, int flags, void* sa, unsigned* salen) {
	if (!isSocketFd(fd)) return -ENOTSOCK;
	Socket* s = fds[fd].sock;
	if (s->domain == AF_PACKET) {
		int ifx, pkttype; uint16_t proto; unsigned char mac[8];
		int n = packetRecv(s, buf, len, flags, &ifx, &proto, &pkttype, mac);
		if (n >= 0 && sa) writeSockaddrLl(sa, salen, proto, ifx, pkttype, mac);
		return n;
	}
	if (s->domain == AF_UNIX) {
		char path[UNIX_PATH_MAX]; unsigned plen = 0;
		int n = unixRecv(s, buf, len, flags, path, &plen);
		if (n >= 0 && sa) writeSockaddrUn(sa, salen, path, plen);
		return n;
	}
	uint32_t ip = 0; uint16_t port = 0;
	int n = socketRecvFrom(s, buf, len, &ip, &port, flags);
	if (n >= 0 && sa) writeSockaddr(sa, salen, ip, port);
	return n;
}

int Syscalls::sockShutdown(int fd, int how) {
	if (!isSocketFd(fd)) return -ENOTSOCK;
	if (fds[fd].sock->type == SOCK_STREAM) tcpShutdown(fds[fd].sock, how);
	return 0;
}

// Network ioctls on a socket fd (Linux SIOC*). `arg` is a struct ifreq: char name[16] then a
// union at offset 16 (sockaddr_in / short flags / int mtu / sockaddr hwaddr).
int Syscalls::netIoctl(int fd, unsigned cmd, void* arg) {
	if (!isSocketFd(fd)) return -ENOTSOCK;
	if (!arg) return -EINVAL;
	unsigned char* ifr = (unsigned char*) arg;
	char name[16]; for (int i = 0; i < 16; i++) name[i] = (char) ifr[i];
	name[15] = 0;
	NetDevice* dev = netByName(name);
	unsigned char* u = ifr + 16;   // the union
	switch (cmd) {
	case 0x8915:  // SIOCGIFADDR
		if (!dev) return -ENXIO;
		{ unsigned l = 16; writeSockaddr(u, &l, dev->ip, 0); } return 0;
	case 0x8916:  // SIOCSIFADDR
		if (!dev) return -ENXIO;
		{ uint32_t ip; uint16_t p; if (parseSockaddr(u, 16, &ip, &p) != SA_AF_INET) return -EAFNOSUPPORT;
		  dev->ip = ip; if (!dev->broadcast) dev->broadcast = (ip & dev->netmask) | ~dev->netmask; } return 0;
	case 0x891c:  // SIOCSIFNETMASK (real Linux value; was mis-numbered 0x891b)
		if (!dev) return -ENXIO;
		{ uint32_t m; uint16_t p; if (parseSockaddr(u, 16, &m, &p) != SA_AF_INET) return -EAFNOSUPPORT;
		  dev->netmask = m; dev->broadcast = (dev->ip & m) | ~m; } return 0;
	case 0x891b:  // SIOCGIFNETMASK (real Linux value; was mis-numbered 0x891a)
		if (!dev) return -ENXIO;
		{ unsigned l = 16; writeSockaddr(u, &l, dev->netmask, 0); } return 0;
	case 0x8919:  // SIOCGIFBRDADDR
		if (!dev) return -ENXIO;
		{ unsigned l = 16; writeSockaddr(u, &l, dev->broadcast, 0); } return 0;
	case 0x8913:  // SIOCGIFFLAGS
		if (!dev) return -ENXIO;
		{ unsigned short fl = 0;
		  if (dev->flags & NETIF_UP) fl |= 0x1;            // IFF_UP
		  if (dev->flags & NETIF_BROADCAST) fl |= 0x2;     // IFF_BROADCAST
		  if (dev->flags & NETIF_LOOPBACK) fl |= 0x8;      // IFF_LOOPBACK
		  if (dev->flags & NETIF_RUNNING) fl |= 0x40;      // IFF_RUNNING
		  u[0] = (unsigned char) fl; u[1] = (unsigned char) (fl >> 8); } return 0;
	case 0x8927:  // SIOCGIFHWADDR -> sa_family ARPHRD_ETHER(1) + 6 MAC bytes
		if (!dev) return -ENXIO;
		u[0] = 1; u[1] = 0; for (int i = 0; i < 6; i++) u[2 + i] = dev->mac[i]; return 0;
	case 0x8921:  // SIOCGIFMTU
		if (!dev) return -ENXIO;
		{ int m = dev->mtu; u[0]=m&0xff; u[1]=(m>>8)&0xff; u[2]=(m>>16)&0xff; u[3]=(m>>24)&0xff; } return 0;
	case 0x8933:  // SIOCGIFINDEX -> ifr_ifindex (int at the union offset)
		if (!dev) return -ENXIO;
		{ int idx = netIfIndexOf(dev); u[0]=idx&0xff; u[1]=(idx>>8)&0xff; u[2]=(idx>>16)&0xff; u[3]=(idx>>24)&0xff; } return 0;
	case 0x890b:    // SIOCADDRT — `arg` is a struct rtentry, not an ifreq
	case 0x890c: {  // SIOCDELRT
		// rtentry (i686): rt_pad1(4) rt_dst(16) rt_gateway(16) rt_genmask(16) rt_flags(u16@52).
		// Each sockaddr_in carries sin_addr at +4, so dst@8 gateway@24 genmask@40 (network order).
		const unsigned char* rt = (const unsigned char*) arg;
		uint32_t dst = rd32be(rt + 8), gw = rd32be(rt + 24), mask = rd32be(rt + 40);
		unsigned flags = rt[52] | (rt[53] << 8);            // RTF_UP=0x1, RTF_GATEWAY=0x2
		NetDevice* egress = netPrimary();                   // single-NIC: the route's device
		if (!egress) return -ENXIO;
		if (cmd == 0x890c) { routeDel(dst, mask); return 0; }
		if (dst == 0 && mask == 0 && (flags & 0x2)) routeAddDefault(egress, gw);
		else routeAdd(dst, mask, (flags & 0x2) ? gw : 0, egress, 0);
		return 0;
	}
	default:
		return -EINVAL;
	}
}

} /* namespace kernel */

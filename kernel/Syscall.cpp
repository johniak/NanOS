#include "Syscall.h"
#include "List.h"
#include "string.h"
#include "Termios.h"
#include <arch/input.h>

namespace kernel {

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
	exited = o.exited;
	exitCode = o.exitCode;
	for (int i = 0; i < MAXFD; i++) {
		fds[i] = o.fds[i];
		if (fds[i].used && fds[i].pipe) {
			if (fds[i].pipeWrite) fds[i].pipe->addWriter();
			else fds[i].pipe->addReader();
		}
	}
}

// Process exit: close any open descriptors so pipe-end refcounts drop (and pipes free at
// zero) — this is what lets a reader see EOF once the last writing process is gone.
Syscalls::~Syscalls() {
	for (int i = 0; i < MAXFD; i++)
		if (fds[i].used && fds[i].pipe)
			close(i);
}

int Syscalls::open(String path, int flags) {
	path = resolvePath(path);   // relative -> against the process cwd (stored absolute on the fd)
	FileStat st;
	bool exists = vfs->stat(path, st) >= 0;
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

int Syscalls::close(int fd) {
	if (!valid(fd))
		return -EBADF;
	if (fds[fd].pipe) {                       // drop this end; free the pipe when both gone
		Pipe* p = fds[fd].pipe;
		if (fds[fd].pipeWrite) p->dropWriter(); else p->dropReader();
		if (p->readers() == 0 && p->writers() == 0)
			delete p;
		fds[fd].pipe = 0;
		fds[fd].pipeWrite = false;
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
	if (fds[dst].pipe) {
		if (fds[dst].pipeWrite) fds[dst].pipe->addWriter();
		else fds[dst].pipe->addReader();
	}
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

int Syscalls::clockGettime(int clkId, unsigned ticks, unsigned realtimeSec, KTimespec* out) {
	// ticks are milliseconds since boot (1000 Hz). CLOCK_REALTIME (0) is wall-clock time:
	// the RTC's whole seconds plus the sub-second part of the monotonic tick. Every other
	// clock id is monotonic: seconds since boot. (CLOCK_REALTIME == 0, MONOTONIC == 1.)
	if (!out)
		return -EINVAL;
	unsigned long long sec = ticks / 1000;
	if (clkId == 0)
		sec = realtimeSec;        // wall clock from the RTC (whole seconds)
	out->tv_sec = (long long) sec;
	out->tv_nsec = (int) ((ticks % 1000) * 1000000u);
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

} /* namespace kernel */

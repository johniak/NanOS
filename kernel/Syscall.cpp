#include "Syscall.h"
#include "List.h"
#include "string.h"
#include <arch/input.h>

namespace kernel {

Syscalls::Syscalls(Vfs* vfs, ConsoleWriteFn cw) {
	this->vfs = vfs;
	this->consoleWrite = cw;
	this->exited = false;
	this->exitCode = 0;
	for (int i = 0; i < MAXFD; i++) {
		fds[i].used = false;
		fds[i].isConsole = false;
		fds[i].offset = 0;
		fds[i].size = 0;
		fds[i].flags = 0;
	}
	// fd 0,1,2 = stdin/stdout/stderr -> console.
	for (int i = 0; i < 3; i++) {
		fds[i].used = true;
		fds[i].isConsole = true;
	}
}

int Syscalls::open(String path, int flags) {
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
			fds[fd].path = path;
			fds[fd].offset = 0;
			fds[fd].size = st.size;
			fds[fd].flags = (unsigned) flags;
			return fd;
		}
	}
	return -EBADF;   // out of descriptors
}

int Syscalls::close(int fd) {
	if (!valid(fd))
		return -EBADF;
	fds[fd].used = false;
	fds[fd].isConsole = false;
	return 0;
}

int Syscalls::read(int fd, void* buf, unsigned n) {
	if (!valid(fd))
		return -EBADF;
	if (fds[fd].isConsole)
		// cooked line or raw bytes; 0 = EOF. O_NONBLOCK -> -EAGAIN instead of blocking.
		return arch::inputRead((char*) buf, n, (fds[fd].flags & O_NONBLOCK) != 0);
	int r = vfs->read(fds[fd].path, n, fds[fd].offset, buf);
	if (r < 0)
		return 0;   // past EOF
	fds[fd].offset += (unsigned) r;
	return r;
}

int Syscalls::write(int fd, const void* buf, unsigned n) {
	if (!valid(fd))
		return -EBADF;
	if (fds[fd].isConsole)
		return consoleWrite((const char*) buf, n);
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
	FileStat st;
	if (vfs->stat(path, st) < 0)
		return -ENOENT;
	fillStat(out, st);
	return 0;
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
int Syscalls::getdents64(int fd, void* buf, unsigned n) {
	if (!valid(fd) || fds[fd].isConsole)
		return -EBADF;
	List<DirEntry> entries;
	if (vfs->readdir(fds[fd].path, entries) < 0)
		return -EBADF;
	char* out = (char*) buf;
	unsigned pos = 0;
	for (int i = 0; i < entries.getCount(); i++) {
		const char* name = entries[i].name;
		unsigned namelen = (unsigned) strlen(name);
		unsigned reclen = (19 + namelen + 1 + 7) & ~7u;
		if (pos + reclen > n)
			break;
		char* rec = out + pos;
		memset(rec, 0, reclen);
		*(unsigned*) (rec + 0) = (unsigned) (i + 1);          // d_ino (low 32)
		*(unsigned short*) (rec + 16) = (unsigned short) reclen; // d_reclen
		rec[18] = (entries[i].type == NODE_DIR) ? 4 : 8;      // d_type (DT_DIR/DT_REG)
		for (unsigned k = 0; k < namelen; k++)
			rec[19 + k] = name[k];
		pos += reclen;
	}
	return (int) pos;
}

int Syscalls::ioctl(int fd, unsigned cmd, void* arg) {
	if (!valid(fd))
		return -EBADF;
	if (fds[fd].isConsole)
		return -EINVAL;          // no console ioctls (yet)
	return vfs->ioctl(fds[fd].path, cmd, arg);
}

int Syscalls::unlink(String path) {
	return vfs->unlink(path);
}

int Syscalls::mkdir(String path, int mode) {
	return vfs->mkdir(path, (unsigned) mode);
}

int Syscalls::fcntl(int fd, int cmd, int arg) {
	if (!valid(fd))
		return -EBADF;
	switch (cmd) {
	case F_GETFL:
		return (int) fds[fd].flags;
	case F_SETFL:
		// Only the file status flags are settable; we track O_NONBLOCK (the rest are
		// stored verbatim but unused). access mode bits are ignored on F_SETFL, per POSIX.
		fds[fd].flags = (unsigned) arg;
		return 0;
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

int Syscalls::clockGettime(int /*clkId*/, unsigned ticks, KTimespec* out) {
	// One monotonic clock (the 1000 Hz scheduler tick); we treat every clk_id the
	// same (REALTIME aliases MONOTONIC since there is no RTC). ticks are milliseconds.
	if (!out)
		return -EINVAL;
	out->tv_sec = (long long) (ticks / 1000);
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

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
	}
	// fd 0,1,2 = stdin/stdout/stderr -> console.
	for (int i = 0; i < 3; i++) {
		fds[i].used = true;
		fds[i].isConsole = true;
	}
}

int Syscalls::open(String path, int /*flags*/) {
	FileStat st;
	if (vfs->stat(path, st) < 0)
		return -ENOENT;
	for (int fd = 3; fd < MAXFD; fd++) {
		if (!fds[fd].used) {
			fds[fd].used = true;
			fds[fd].isConsole = false;
			fds[fd].path = path;
			fds[fd].offset = 0;
			fds[fd].size = st.size;
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
		return arch::inputReadLine((char*) buf, n);   // blocking cooked line; 0 = EOF
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
	return -EROFS;   // filesystem is read-only
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

int Syscalls::fstat(int fd, LinuxStat* out) {
	if (!valid(fd))
		return -EBADF;
	if (fds[fd].isConsole) {
		out->st_mode = 0x2000;   // S_IFCHR
		out->st_size = 0;
		return 0;
	}
	FileStat st;
	if (vfs->stat(fds[fd].path, st) < 0)
		return -EBADF;
	out->st_mode = (st.type == NODE_DIR) ? 0x4000 : 0x8000;   // S_IFDIR / S_IFREG
	out->st_size = st.size;
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

void Syscalls::exit(int code) {
	exited = true;
	exitCode = code;
}

} /* namespace kernel */

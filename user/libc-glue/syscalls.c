/*
 * syscalls.c — the NanOS porting layer for picolibc.
 *
 * picolibc supplies the portable upper half (stdio/printf/malloc/qsort/string/
 * time); this file supplies the thin bottom that talks to the kernel via int 0x80.
 * picolibc's POSIX layer references these names WITHOUT a leading underscore
 * (write/read/.../sbrk), plus _exit. struct stat is the glibc-style layout; we map
 * the kernel's compact LinuxStat onto it.
 */
#include "SyscallNr.h"
#include <errno.h>
#include <stddef.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <poll.h>

/* Force the stdin stream object to be linked. picolibc's tinystdio pulls stdin/stdout/
 * stderr from libc.a only on reference; programs here use stdout/stderr (printf) but
 * rarely stdin, yet fread()'s __bufio_get references stdin weakly (it flushes stdout
 * before reading stdin). Without a strong reference, stdin resolves to address 0 and the
 * first fread on ANY file dereferences NULL. A static initializer won't do (a stream
 * isn't a compile-time constant), so reference it from a (linked, never-called) function
 * — the relocation alone pulls picolibc's stdin object in. */
FILE* __nx_keep_stdin;
void __nx_link_streams(void) { __nx_keep_stdin = stdin; }

static inline int sys3(int nr, int a, int b, int c) {
	int r;
	__asm__ __volatile__("int $0x80" : "=a"(r) : "a"(nr), "b"(a), "c"(b), "d"(c) : "memory");
	return r;
}

/* Negative kernel return -> errno + -1, matching the POSIX contract picolibc expects. */
static int reterr(int r) {
	if (r < 0) { errno = -r; return -1; }
	return r;
}

int write(int fd, const void* b, int n) { return reterr(sys3(SYS_write, fd, (int) b, n)); }
int read(int fd, void* b, int n)        { return reterr(sys3(SYS_read, fd, (int) b, n)); }
int open(const char* p, int fl, ...) {
	char abs[256];
	nx_resolve(p, abs);
	return reterr(sys3(SYS_open, (int) abs, fl, 0));
}
int close(int fd)                       { return reterr(sys3(SYS_close, fd, 0, 0)); }
/* pipe/dup/dup2: descriptor plumbing for shells (pipelines, redirection) and the terminal
 * (the child puts the pty slave on fd 0/1/2 via dup2). */
int pipe(int fd[2])                     { return reterr(sys3(SYS_pipe, (int) fd, 0, 0)); }
int dup(int fd)                         { return reterr(sys3(SYS_dup, fd, 0, 0)); }
int dup2(int o, int n)                  { return reterr(sys3(SYS_dup2, o, n, 0)); }
/* poll(2): wait until one of the fds is ready (the terminal emulator's event loop, later
 * htop/btop). timeout in ms (-1 = block forever, 0 = return immediately). */
int poll(struct pollfd* fds, nfds_t nfds, int timeout) {
	return reterr(sys3(SYS_poll, (int) fds, (int) nfds, timeout));
}
/* fcntl(2): we support F_GETFL/F_SETFL (the O_NONBLOCK status flag) for non-blocking
 * console reads. The third argument is an int (the flags for F_SETFL). */
int fcntl(int fd, int cmd, ...) {
	va_list ap;
	va_start(ap, cmd);
	int arg = va_arg(ap, int);
	va_end(ap);
	return reterr(sys3(SYS_fcntl, fd, cmd, arg));
}
int lseek(int fd, int off, int wh)      { return reterr(sys3(SYS_lseek, fd, off, wh)); }
int unlink(const char* p) {
	char abs[256];
	nx_resolve(p, abs);
	return reterr(sys3(SYS_unlink, (int) abs, 0, 0));
}
int mkdir(const char* p, mode_t mode) {
	char abs[256];
	nx_resolve(p, abs);
	return reterr(sys3(SYS_mkdir, (int) abs, (int) mode, 0));
}
/* rename(2): NanOS has no rename syscall, so do it in userland — copy the old file to the
 * new name, then unlink the old. Both ends are ordinary files (Doom uses it to finalize a
 * savegame from a temp file). Only valid within a writable fs (e.g. /tmp). */
int rename(const char* oldp, const char* newp) {
	int in = open(oldp, 0 /*O_RDONLY*/);
	if (in < 0) return -1;
	int out = open(newp, 01 | 0100 | 01000 /*O_WRONLY|O_CREAT|O_TRUNC*/, 0644);
	if (out < 0) { close(in); return -1; }
	char buf[512];
	int n;
	while ((n = read(in, buf, sizeof buf)) > 0) {
		int off = 0;
		while (off < n) {
			int w = write(out, buf + off, n - off);
			if (w <= 0) { close(in); close(out); return -1; }
			off += w;
		}
	}
	close(in);
	close(out);
	if (n < 0) return -1;
	unlink(oldp);
	return 0;
}
void _exit(int c)                       { sys3(SYS_exit, c, 0, 0); for (;;) {} }
/* isatty(2): a fd is a terminal iff TCGETS (tcgetattr) succeeds on it — exactly how glibc
 * decides. Console + pty-slave answer it; files/pipes/fb0 do not (-> errno ENOTTY). */
int isatty(int fd) {
	char termios_buf[64];
	if (sys3(SYS_ioctl, fd, 0x5401 /* TCGETS */, (int) termios_buf) == 0)
		return 1;
	errno = ENOTTY;
	return 0;
}
int getpid(void)                        { return reterr(sys3(SYS_getpid, 0, 0, 0)); }
int getppid(void)                       { return reterr(sys3(SYS_getppid, 0, 0, 0)); }
int kill(int p, int s)                  { return reterr(sys3(SYS_kill, p, s, 0)); }

/* Sessions + process groups (job control): the shell uses these to put each job in its own
 * group and hand the terminal to the foreground group, so Ctrl+C hits the whole job. */
int setpgid(int pid, int pgid)          { return reterr(sys3(SYS_setpgid, pid, pgid, 0)); }
int getpgid(int pid)                    { return reterr(sys3(SYS_getpgid, pid, 0, 0)); }
int getpgrp(void)                       { return reterr(sys3(SYS_getpgrp, 0, 0, 0)); }
int setsid(void)                        { return reterr(sys3(SYS_setsid, 0, 0, 0)); }
int getsid(int pid)                     { return reterr(sys3(SYS_getsid, pid, 0, 0)); }

/* signal(2): install a disposition. We pass the libc sigreturn trampoline as the
 * kernel's sa_restorer; the kernel runs the handler in ring 3 and returns through it.
 * Returns the previous disposition, or SIG_ERR on error. */
extern void __nx_sigtramp(void);
void (*signal(int sig, void (*handler)(int)))(int) {
	int r = sys3(SYS_signal, sig, (int) handler, (int) &__nx_sigtramp);
	if (r < 0) { errno = -r; return (void (*)(int)) -1; }   /* SIG_ERR */
	return (void (*)(int)) r;
}
int times(void* b)                      { (void) b; return 0; }

/* Monotonic clock from the kernel (1000 Hz scheduler tick). The kernel timespec is
 * {int tv_sec; int tv_nsec;}, the same 8-byte layout as picolibc's on i386, so we hand
 * it the struct pointer directly. */
int clock_gettime(clockid_t clk, struct timespec* tp) {
	return reterr(sys3(SYS_clock_gettime, (int) clk, (int) tp, 0));
}

/* nanosleep(2): block for the requested duration; rem (if given) gets the unslept
 * remainder when interrupted by a signal. */
int nanosleep(const struct timespec* req, struct timespec* rem) {
	return reterr(sys3(SYS_nanosleep, (int) req, (int) rem, 0));
}

/* gettimeofday now reads the monotonic clock (offset by a fixed epoch so ls -l still
 * renders a plausible wall-clock timestamp). */
int gettimeofday(struct timeval* tv, void* tz) {
	(void) tz;
	if (tv) {
		struct timespec ts = { 0, 0 };
		clock_gettime(0, &ts);
		tv->tv_sec = 1700000000 + ts.tv_sec;
		tv->tv_usec = ts.tv_nsec / 1000;
	}
	return 0;
}

/* fork(2): returns the child pid to the parent, 0 in the child, -1 on failure. */
int fork(void) {
	return reterr(sys3(SYS_fork, 0, 0, 0));
}

/* waitpid(2): block for a child to exit; *status gets a WEXITSTATUS-style code. */
int waitpid(int pid, int* status, int options) {
	return reterr(sys3(SYS_waitpid, pid, (int) status, options));
}

/* The process environment. getenv()/setenv() (picolibc) read `environ` directly — both
 * live here in libc.ndl, so that reference is module-local (no import). crt0 lives in the
 * program ELF and cannot reach a data symbol across the .ndl boundary by name (only the
 * Windows-style __imp_ slot), so it publishes envp through this exported FUNCTION instead,
 * which functions import cleanly via the jmp thunk. */
char** environ = 0;
void __nx_set_environ(char** e) { environ = e; }

/* Replace the current process image with <path>. On success it does not return (the
 * kernel rewrites the trap frame so the iret lands in the new program); on failure it
 * returns -1 with errno set. A NULL envp inherits the caller's current environment. */
int execve(const char* path, char* const argv[], char* const envp[]) {
	char abs[256];
	nx_resolve(path, abs);
	if (!envp) envp = environ;
	return reterr(sys3(SYS_execve, (int) abs, (int) argv, (int) envp));
}

/* Console input mode: 0 = cooked (line-edited), 1 = raw (per-key). The shell uses
 * raw for its own line editor and cooked while a child program runs. */
int termmode(int raw) {
	return sys3(SYS_termmode, raw, 0, 0);
}

/* ioctl(2): used for the framebuffer (FBIOGET_*SCREENINFO). The single pointer arg is
 * passed through to the device. */
int ioctl(int fd, unsigned long request, ...) {
	va_list ap;
	va_start(ap, request);
	void* arg = va_arg(ap, void*);
	va_end(ap);
	return reterr(sys3(SYS_ioctl, fd, (int) request, (int) arg));
}

/* mmap(2): we repack the six POSIX arguments into our 3-register kernel ABI
 * (fd, length, offset); only shared device mappings (e.g. /dev/fb0) are supported, so
 * addr/prot/flags are ignored. Returns MAP_FAILED ((void*)-1) on error. */
void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
	(void) addr; (void) prot; (void) flags;
	int r = sys3(SYS_mmap2, fd, (int) length, (int) offset);
	if (r < 0) { errno = -r; return (void*) -1; }
	return (void*) r;
}

/* brk(2)/sbrk(2): the heap is a growable high-VA region the kernel maps on demand (see
 * kernel SYS_brk / arch mmuSetUserBrk). brk(0) reports the current break; brk(addr) sets
 * it and returns the resulting break (the OLD break on failure). sbrk tracks the break in
 * userland and grows/shrinks via brk. picolibc's malloc sits directly on top of this. */
static char* nx_brk(char* addr) { return (char*) sys3(SYS_brk, (int) addr, 0, 0); }
void* sbrk(int incr) {
	static char* cur = 0;
	if (!cur)
		cur = nx_brk(0);                   /* learn the initial break (NX_BRK_BASE) */
	char* want = cur + incr;
	char* got = nx_brk(want);
	if (got != want) { errno = ENOMEM; return (void*) -1; }
	char* prev = cur;
	cur = want;
	return prev;
}

/* Map the kernel's compact stat onto picolibc's struct stat. Zero the whole struct first
 * (the kernel fills only a few fields) and set a sane st_blksize — picolibc's stdio sizes
 * its file buffer from st_blksize, so leaving it as stack garbage makes fopen try to
 * malloc a huge (or zero) buffer, which fails and then crashes on the first fread. */
struct knl_stat { unsigned mode, size, nlink, uid, gid, mtime, ino; };
static void fillstat(struct stat* o, const struct knl_stat* k) {
	memset(o, 0, sizeof *o);
	o->st_mode = k->mode;
	o->st_size = k->size;
	o->st_nlink = k->nlink;
	o->st_uid = k->uid;
	o->st_gid = k->gid;
	o->st_mtime = k->mtime;
	o->st_ino = k->ino;
	o->st_blksize = 512;
}
int stat(const char* p, struct stat* o) {
	char abs[256];
	nx_resolve(p, abs);
	struct knl_stat k;
	int r = sys3(SYS_stat, (int) abs, (int) &k, 0);
	if (r < 0) { errno = -r; return -1; }
	fillstat(o, &k);
	return 0;
}
int lstat(const char* p, struct stat* o) { return stat(p, o); }   /* no symlinks */
int fstat(int fd, struct stat* o) {
	struct knl_stat k;
	int r = sys3(SYS_fstat, fd, (int) &k, 0);
	if (r < 0) { errno = -r; return -1; }
	fillstat(o, &k);
	return 0;
}

/* picolibc supplies default no-op __retarget_lock_* (single-threaded), so we do
 * not define them here. */

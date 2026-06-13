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
#include <stdlib.h>
#include <signal.h>
#include <poll.h>
#include <utime.h>

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
	return reterr(sys3(SYS_open, (int) p, fl, 0));   // kernel resolves relative paths vs the cwd
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
/* select(2) over poll: NanOS has no select syscall, but readline/bash use select() to wait
 * for input. Translate the fd_sets into pollfds (up to 64 fds — ample for a shell), poll,
 * then translate the ready set back. Returns the number of ready fds. */
#include <sys/select.h>
int select(int nfds, fd_set* r, fd_set* w, fd_set* e, struct timeval* tv) {
	struct pollfd pf[64];
	int map[64], n = 0;
	if (nfds > 64) nfds = 64;
	(void) e;   /* exceptfds: NanOS poll has no out-of-band class; report none ready */
	for (int fd = 0; fd < nfds; fd++) {
		short ev = 0;
		if (r && FD_ISSET(fd, r)) ev |= POLLIN;
		if (w && FD_ISSET(fd, w)) ev |= POLLOUT;
		if (ev) { pf[n].fd = fd; pf[n].events = ev; pf[n].revents = 0; map[n] = fd; n++; }
	}
	int timeout = tv ? (int) (tv->tv_sec * 1000 + tv->tv_usec / 1000) : -1;
	int pr = poll(pf, (nfds_t) n, timeout);
	if (pr < 0) return -1;
	if (r) FD_ZERO(r);
	if (w) FD_ZERO(w);
	if (e) FD_ZERO(e);
	int count = 0;
	for (int i = 0; i < n; i++) {
		if (r && (pf[i].revents & (POLLIN | POLLHUP | POLLERR))) { FD_SET(map[i], r); count++; }
		if (w && (pf[i].revents & POLLOUT)) { FD_SET(map[i], w); count++; }
	}
	return count;
}
/* fcntl(2): F_GETFL/F_SETFL (O_NONBLOCK), F_GETFD/F_SETFD (FD_CLOEXEC), and
 * F_DUPFD/F_DUPFD_CLOEXEC (duplicate to the lowest fd >= arg). The third argument is an
 * int (flags for F_SET*, the fd floor for F_DUPFD); harmless for the no-arg F_GET* forms. */
int fcntl(int fd, int cmd, ...) {
	va_list ap;
	va_start(ap, cmd);
	int arg = va_arg(ap, int);
	va_end(ap);
	return reterr(sys3(SYS_fcntl, fd, cmd, arg));
}
int lseek(int fd, int off, int wh)      { return reterr(sys3(SYS_lseek, fd, off, wh)); }
int unlink(const char* p) {
	return reterr(sys3(SYS_unlink, (int) p, 0, 0));
}
int link(const char* oldp, const char* newp) {
	return reterr(sys3(SYS_link, (int) oldp, (int) newp, 0));
}
int mkdir(const char* p, mode_t mode) {
	return reterr(sys3(SYS_mkdir, (int) p, (int) mode, 0));
}
int rmdir(const char* p)                { return reterr(sys3(SYS_rmdir, (int) p, 0, 0)); }
int fsync(int fd)                       { return reterr(sys3(SYS_fsync, fd, 0, 0)); }
int fdatasync(int fd)                   { return reterr(sys3(SYS_fdatasync, fd, 0, 0)); }
void sync(void)                         { sys3(SYS_sync, 0, 0, 0); }
int fchdir(int fd)                      { return reterr(sys3(SYS_fchdir, fd, 0, 0)); }
int ftruncate(int fd, off_t length)     { return reterr(sys3(SYS_ftruncate, fd, (int) length, 0)); }
int truncate(const char* p, off_t length) { return reterr(sys3(SYS_truncate, (int) p, (int) length, 0)); }
/* utime(2): the kernel reads struct utimbuf {time_t actime, modtime} directly (NULL -> now). */
int utime(const char* path, const struct utimbuf* times) {
	return reterr(sys3(SYS_utime, (int) path, (int) times, 0));
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
/* raise(3): defined here (not pulled from picolibc) because picolibc's signal.c bundles
 * raise WITH signal, which we override — so importing raise would drag a conflicting signal.
 * As a glue symbol it is auto-excluded from the picolibc auto-export. */
int raise(int s)                        { return kill(getpid(), s); }

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

/* sigaction(2): implemented over SYS_signal. The struct layout lives in picolibc's headers
 * (compiled here), so sa_handler/sa_mask read at the right offsets. Our kernel signals
 * always carry SA_RESTART (BSD semantics) and auto-handle the trampoline, so sa_flags/
 * sa_mask beyond the handler are not separately honored. act == NULL queries without
 * changing (handler sentinel -1). Returns the previous handler in `old`. */
int sigaction(int sig, const struct sigaction* act, struct sigaction* old) {
	int prev = act
		? sys3(SYS_signal, sig, (int) (size_t) act->sa_handler, (int) &__nx_sigtramp)
		: sys3(SYS_signal, sig, -1 /* query, do not change */, 0);
	if (prev < 0 && prev > -256) { errno = -prev; return -1; }
	if (old) {
		old->sa_handler = (void (*)(int)) (size_t) prev;
		old->sa_flags = 0;
		memset(&old->sa_mask, 0, sizeof old->sa_mask);
	}
	return 0;
}

/* sigprocmask(2): sigset_t is a 32-bit unsigned long here, so it maps straight onto the
 * kernel's bitmask. picolibc uses BSD `how` values (SETMASK=0, BLOCK=1, UNBLOCK=2) but the
 * kernel uses the Linux numbering (BLOCK=0, UNBLOCK=1, SETMASK=2), so translate. A NULL set
 * queries without changing (BLOCK an empty set). */
int sigprocmask(int how, const sigset_t* set, sigset_t* old) {
	int khow = set ? (how == SIG_BLOCK ? 0 : how == SIG_UNBLOCK ? 1 : 2) : 0;
	unsigned o = 0;
	int r = sys3(SYS_sigprocmask, khow, set ? (int) (unsigned) *set : 0, (int) &o);
	if (r < 0) { errno = -r; return -1; }
	if (old) *old = (sigset_t) o;
	return 0;
}

/* pause(2): block until a signal handler runs. Always returns -1 with errno == EINTR (the
 * kernel never restarts pause). */
int pause(void) {
	return reterr(sys3(SYS_pause, 0, 0, 0));
}

/* sigsuspend(2): install *mask as the blocked set, wait for a deliverable signal, restore the
 * previous mask. sigset_t is one 32-bit word here (same bit layout as the kernel's mask), so
 * the value passes straight through in the first arg. Always returns -1 with errno == EINTR. */
int sigsuspend(const sigset_t* mask) {
	return reterr(sys3(SYS_sigsuspend, mask ? (int) (unsigned) *mask : 0, 0, 0));
}

/* getentropy(3): no hardware RNG; fill the buffer from a weak LCG seeded once. NOT
 * cryptographically secure — just enough for picolibc's arc4random seeding to run. */
int getentropy(void* buf, size_t n) {
	static unsigned seed = 0x9e3779b9u;
	unsigned char* p = (unsigned char*) buf;
	if (n > 256) { errno = EIO; return -1; }   /* getentropy caps at 256 bytes */
	for (size_t i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; p[i] = (unsigned char) (seed >> 16); }
	return 0;
}
/* times(): the kernel fills the struct tms (utime/stime, child times 0) and returns the
 * monotonic tick count. Real per-process CPU accounting, not a 0 stub. */
int times(void* b)                      { return sys3(SYS_times, (int) b, 0, 0); }

/* Monotonic clock from the kernel (1000 Hz scheduler tick). The kernel timespec is
 * {int tv_sec; int tv_nsec;}, the same 8-byte layout as picolibc's on i386, so we hand
 * it the struct pointer directly. */
int clock_gettime(clockid_t clk, struct timespec* tp) {
	return reterr(sys3(SYS_clock_gettime, (int) clk, (int) tp, 0));
}

/* clock_getres(2): the kernel clock advances on the 1000 Hz scheduler tick, so the resolution
 * is 1 ms for every supported clock. No kernel syscall — the value is fixed. */
int clock_getres(clockid_t clk, struct timespec* res) {
	(void) clk;
	if (res) { res->tv_sec = 0; res->tv_nsec = 1000000; }   /* 1 ms */
	return 0;
}

/* nanosleep(2): block for the requested duration; rem (if given) gets the unslept
 * remainder when interrupted by a signal. */
int nanosleep(const struct timespec* req, struct timespec* rem) {
	return reterr(sys3(SYS_nanosleep, (int) req, (int) rem, 0));
}

/* gettimeofday reads CLOCK_REALTIME (clk 0), which the kernel backs with the CMOS RTC,
 * so tv_sec is the real Unix time — no fabricated fixed epoch. */
int gettimeofday(struct timeval* tv, void* tz) {
	(void) tz;
	if (tv) {
		struct timespec ts = { 0, 0 };
		clock_gettime(0, &ts);          /* CLOCK_REALTIME */
		tv->tv_sec = ts.tv_sec;
		tv->tv_usec = ts.tv_nsec / 1000;
	}
	return 0;
}

/* fork(2): returns the child pid to the parent, 0 in the child, -1 on failure. */
int fork(void) {
	return reterr(sys3(SYS_fork, 0, 0, 0));
}

/* vfork(2): NanOS has no copy-on-write vfork; a full fork is a correct (if heavier) substitute —
 * the child fork()s and immediately exec()s or _exit()s, exactly the vfork contract. */
int vfork(void) {
	return reterr(sys3(SYS_fork, 0, 0, 0));
}

/* waitpid(2): block for a child to exit; *status gets a WEXITSTATUS-style code. */
int waitpid(int pid, int* status, int options) {
	return reterr(sys3(SYS_waitpid, pid, (int) status, options));
}

/* wait(2): reap any child (waitpid(-1, ...) without options). inetd's SIGCHLD reaper and
 * libinetutils' ttymsg use this. */
int wait(int* status) {
	return waitpid(-1, status, 0);
}

/* The process environment. getenv() (picolibc) reads `environ` directly — both live here
 * in libc.ndl, so that reference is module-local (no import). crt0 lives in the program ELF
 * and cannot reach a data symbol across the .ndl boundary by name (only the Windows-style
 * __imp_ slot), so it publishes envp through this exported FUNCTION instead, which functions
 * import cleanly via the jmp thunk. */
char** environ = 0;
void __nx_set_environ(char** e) { environ = e; }

/* Environment mutation (setenv/unsetenv/putenv). We define these here rather than use
 * picolibc's because the initial `environ` points at the argv+envp image on the user stack,
 * not the heap — picolibc's would try to free()/realloc() those entries. On the first
 * mutation we copy to a heap-owned NULL-terminated array we fully control; getenv() keeps
 * seeing the same `environ`. (Replaced entries are intentionally leaked — bounded + simple.) */
static int env_owned = 0;

static int env_count(void) {
	int n = 0;
	if (environ) while (environ[n]) n++;
	return n;
}

static int env_find(const char* name, int* nlOut) {
	int nl = 0;
	while (name[nl] && name[nl] != '=') nl++;
	if (nlOut) *nlOut = nl;
	if (environ)
		for (int i = 0; environ[i]; i++)
			if (strncmp(environ[i], name, nl) == 0 && environ[i][nl] == '=')
				return i;
	return -1;
}

/* Make `environ` a heap array with room for `extra` more entries (plus the NULL slot). */
static int env_reserve(int extra) {
	int n = env_count();
	char** arr = (char**) malloc((n + extra + 1) * sizeof(char*));
	if (!arr) { errno = ENOMEM; return -1; }
	for (int i = 0; i < n; i++) arr[i] = environ[i];
	arr[n] = 0;
	if (env_owned) free(environ);
	environ = arr;
	env_owned = 1;
	return 0;
}

int setenv(const char* name, const char* value, int overwrite) {
	if (!name || !*name || strchr(name, '=')) { errno = EINVAL; return -1; }
	int nl, idx = env_find(name, &nl);
	if (idx >= 0 && !overwrite) return 0;
	int vl = (int) strlen(value);
	char* entry = (char*) malloc(nl + 1 + vl + 1);
	if (!entry) { errno = ENOMEM; return -1; }
	memcpy(entry, name, nl);
	entry[nl] = '=';
	memcpy(entry + nl + 1, value, vl + 1);
	if (idx >= 0) {
		if (!env_owned && env_reserve(0) < 0) { free(entry); return -1; }
		environ[idx] = entry;
		return 0;
	}
	if (env_reserve(1) < 0) { free(entry); return -1; }
	int n = env_count();
	environ[n] = entry;
	environ[n + 1] = 0;
	return 0;
}

int unsetenv(const char* name) {
	if (!name || !*name || strchr(name, '=')) { errno = EINVAL; return -1; }
	int nl, idx = env_find(name, &nl);
	if (idx < 0) return 0;
	if (!env_owned && env_reserve(0) < 0) return -1;
	for (int i = idx; environ[i]; i++) environ[i] = environ[i + 1];   /* shift incl. NULL */
	return 0;
}

int putenv(char* str) {
	/* POSIX: `str` becomes part of the environment (caller must keep it alive). */
	int nl, idx = env_find(str, &nl);
	if (idx >= 0) {
		if (!env_owned && env_reserve(0) < 0) return -1;
		environ[idx] = str;
		return 0;
	}
	if (env_reserve(1) < 0) return -1;
	int n = env_count();
	environ[n] = str;
	environ[n + 1] = 0;
	return 0;
}

/* Replace the current process image with <path>. On success it does not return (the
 * kernel rewrites the trap frame so the iret lands in the new program); on failure it
 * returns -1 with errno set. A NULL envp inherits the caller's current environment. */
int execve(const char* path, char* const argv[], char* const envp[]) {
	if (!envp) envp = environ;
	return reterr(sys3(SYS_execve, (int) path, (int) argv, (int) envp));
}

/* execv(3): exec with the caller's current environment (picolibc ships only execve). inetd's
 * service launcher (execv(se_server, se_argv)) goes through here. */
int execv(const char* path, char* const argv[]) {
	return execve(path, argv, environ);
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

/* 5-argument syscall (ebx/ecx/edx/esi/edi) for mmap, which needs more than three args. */
static inline int sys5(int nr, int a, int b, int c, int d, int e) {
	int r;
	__asm__ __volatile__("int $0x80"
		: "=a"(r) : "a"(nr), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e) : "memory");
	return r;
}

/* mmap(2): pass length/prot/flags/fd/offset to the kernel. Supports device mappings
 * (e.g. /dev/fb0), anonymous mappings (fd < 0), and file-backed mappings (regular-file fd,
 * eagerly loaded). `addr` is advisory and ignored (the kernel picks the VA). Returns
 * MAP_FAILED ((void*)-1) on error. */
void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
	(void) addr; (void) flags;
	int r = sys5(SYS_mmap2, (int) length, prot, flags, fd, (int) offset);
	if (r < 0) { errno = -r; return (void*) -1; }
	return (void*) r;
}

/* munmap(2): the kernel's mmap eagerly backs mappings with allocated pages and has no unmap
 * syscall yet, so this is a no-op (the region is reclaimed when the process exits). Safe for
 * the short-lived tools that mmap-then-process-then-exit (e.g. wget reading a local file). */
int munmap(void* addr, size_t length) { (void) addr; (void) length; return 0; }

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
	struct knl_stat k;
	int r = sys3(SYS_stat, (int) p, (int) &k, 0);
	if (r < 0) { errno = -r; return -1; }
	fillstat(o, &k);
	return 0;
}
/* lstat: stat the link itself (the kernel resolves all but the final component). */
int lstat(const char* p, struct stat* o) {
	struct knl_stat k;
	int r = sys3(SYS_lstat, (int) p, (int) &k, 0);
	if (r < 0) { errno = -r; return -1; }
	fillstat(o, &k);
	return 0;
}
int fstat(int fd, struct stat* o) {
	struct knl_stat k;
	int r = sys3(SYS_fstat, fd, (int) &k, 0);
	if (r < 0) { errno = -r; return -1; }
	fillstat(o, &k);
	return 0;
}

/* picolibc supplies default no-op __retarget_lock_* (single-threaded), so we do
 * not define them here. */

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
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>

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
int lseek(int fd, int off, int wh)      { return reterr(sys3(SYS_lseek, fd, off, wh)); }
void _exit(int c)                       { sys3(SYS_exit, c, 0, 0); for (;;) {} }
int isatty(int fd)                      { return fd == 0 || fd == 1 || fd == 2; }
int getpid(void)                        { return 1; }
int kill(int p, int s)                  { return reterr(sys3(SYS_kill, p, s, 0)); }

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

/* No real-time clock: a fixed epoch so ls -l renders a stable timestamp. */
int gettimeofday(struct timeval* tv, void* tz) {
	(void) tz;
	if (tv) { tv->tv_sec = 1700000000; tv->tv_usec = 0; }
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

/* Replace the current process image with <path>. On success it does not return (the
 * kernel rewrites the trap frame so the iret lands in the new program); on failure it
 * returns -1 with errno set. envp is accepted for the POSIX signature but unused. */
int execve(const char* path, char* const argv[], char* const envp[]) {
	(void) envp;
	char abs[256];
	nx_resolve(path, abs);
	return reterr(sys3(SYS_execve, (int) abs, (int) argv, 0));
}

/* Console input mode: 0 = cooked (line-edited), 1 = raw (per-key). The shell uses
 * raw for its own line editor and cooked while a child program runs. */
int termmode(int raw) {
	return sys3(SYS_termmode, raw, 0, 0);
}

/* Single fixed heap window, mapped by the kernel at exec time: [0x480000,0x4F0000). */
void* sbrk(int incr) {
	static char* cur = (char*) 0x480000;   /* NX_HEAP_BASE */
	char* top = (char*) 0x4F0000;          /* just below the user stack window */
	if (cur + incr > top) { errno = ENOMEM; return (void*) -1; }
	char* prev = cur;
	cur += incr;
	return prev;
}

/* Map the kernel's compact stat onto picolibc's struct stat. */
struct knl_stat { unsigned mode, size, nlink, uid, gid, mtime, ino; };
static void fillstat(struct stat* o, const struct knl_stat* k) {
	o->st_mode = k->mode;
	o->st_size = k->size;
	o->st_nlink = k->nlink;
	o->st_uid = k->uid;
	o->st_gid = k->gid;
	o->st_mtime = k->mtime;
	o->st_ino = k->ino;
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

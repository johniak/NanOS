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
#include <stdint.h>
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

/* Force ALL THREE standard stream objects to be linked into libc.ndl. picolibc's tinystdio
 * declares stdin/stdout/stderr as `FILE *const` pointer variables pulled from libc.a only on
 * reference; libc.ndl exports a symbol only if its defining object is pulled in. Without a strong
 * reference here, the unreferenced streams stay undefined and a program importing one resolves it
 * to ADDRESS 0 — so e.g. `fprintf(stderr, ...)` faults taking &stderr (seen with Dropbear, whose
 * dropbearkey logs to stderr but never touches stdin/stdout). Referencing all three from a
 * (linked, never-called) function pulls every stream object in, so libc.ndl exports all three. */
FILE* __nx_keep_stdin;
FILE* __nx_keep_stdout;
FILE* __nx_keep_stderr;
void __nx_link_streams(void) {
	__nx_keep_stdin = stdin;
	__nx_keep_stdout = stdout;
	__nx_keep_stderr = stderr;
}

/* The kernel trap is arch-specific: i386 uses int 0x80 (nr=eax, args ebx/ecx/edx/esi/edi);
 * x86_64 uses the SYSCALL instruction (nr=rax, args rdi/rsi/rdx/r10/r8/r9 — SYSCALL clobbers
 * rcx/r11). The `int` arguments are widened to `long` for the 64-bit registers; pointers reach
 * here already truncated to `int` by the wrappers below, so they must live in the low 2 GiB —
 * which the x86_64 user memory map guarantees (fixed low base, low stack + mmap window). */
static inline int sys3(int nr, int a, int b, int c) {
	int r;
#if defined(__x86_64__)
	long rr;
	__asm__ __volatile__("syscall" : "=a"(rr)
		: "a"((long) nr), "D"((long) a), "S"((long) b), "d"((long) c)
		: "rcx", "r11", "memory");
	r = (int) rr;
#else
	__asm__ __volatile__("int $0x80" : "=a"(r) : "a"(nr), "b"(a), "c"(b), "d"(c) : "memory");
#endif
	return r;
}

/* 4-arg form (i386: nr=eax, a=ebx, b=ecx, c=edx, d=esi; x86_64: 4th arg in r10). Needed by
 * the rt_sig* calls, whose 4th argument is the sigsetsize. */
static inline int sys4(int nr, int a, int b, int c, int d) {
	int r;
#if defined(__x86_64__)
	long rr;
	register long r10 __asm__("r10") = (long) d;
	__asm__ __volatile__("syscall" : "=a"(rr)
		: "a"((long) nr), "D"((long) a), "S"((long) b), "d"((long) c), "r"(r10)
		: "rcx", "r11", "memory");
	r = (int) rr;
#else
	__asm__ __volatile__("int $0x80"
		: "=a"(r) : "a"(nr), "b"(a), "c"(b), "d"(c), "S"(d) : "memory");
#endif
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

/* ---- process credentials: real syscalls so userland sees + changes the kernel's per-process
 * Cred (these used to be single-user-root stubs in posixstubs.c). getuid/getgid never fail. */
uid_t getuid(void)   { return (uid_t) sys3(SYS_getuid, 0, 0, 0); }
uid_t geteuid(void)  { return (uid_t) sys3(SYS_geteuid, 0, 0, 0); }
gid_t getgid(void)   { return (gid_t) sys3(SYS_getgid, 0, 0, 0); }
gid_t getegid(void)  { return (gid_t) sys3(SYS_getegid, 0, 0, 0); }
int setuid(uid_t u)  { return reterr(sys3(SYS_setuid, (int) u, 0, 0)); }
int setgid(gid_t g)  { return reterr(sys3(SYS_setgid, (int) g, 0, 0)); }
/* The POSIX "leave this id unchanged" sentinel is (uid_t)-1. picolibc's uid_t/gid_t are 16-bit,
 * so (uid_t)-1 == 65535 — a plain (int) cast would hand the kernel 65535 (a real id), but the
 * kernel recognises ONLY -1 as "unchanged". Map the sentinel to -1 here so the setres/setre
 * family matches POSIX. (This was the sudo set_perms bug: it passed (uid_t)-1 and the kernel
 * changed the id to 65535 instead of leaving it unchanged.) */
static int idarg(uid_t v) { return v == (uid_t) -1 ? -1 : (int) v; }
int seteuid(uid_t u) { return reterr(sys3(SYS_setresuid, -1, idarg(u), -1)); }   /* glibc routes via setresuid */
int setegid(gid_t g) { return reterr(sys3(SYS_setresgid, -1, idarg(g), -1)); }
int setreuid(uid_t r, uid_t e) { return reterr(sys3(SYS_setreuid, idarg(r), idarg(e), 0)); }
int setregid(gid_t r, gid_t e) { return reterr(sys3(SYS_setregid, idarg(r), idarg(e), 0)); }
int setresuid(uid_t r, uid_t e, uid_t s) { return reterr(sys3(SYS_setresuid, idarg(r), idarg(e), idarg(s))); }
int setresgid(gid_t r, gid_t e, gid_t s) { return reterr(sys3(SYS_setresgid, idarg(r), idarg(e), idarg(s))); }
int getresuid(uid_t* r, uid_t* e, uid_t* s) { return reterr(sys3(SYS_getresuid, (int) r, (int) e, (int) s)); }
int getresgid(gid_t* r, gid_t* e, gid_t* s) { return reterr(sys3(SYS_getresgid, (int) r, (int) e, (int) s)); }
int setfsuid(uid_t u) { return sys3(SYS_setfsuid, idarg(u), 0, 0); }   /* returns the PREVIOUS fsuid */
int setfsgid(gid_t g) { return sys3(SYS_setfsgid, idarg(g), 0, 0); }
int getgroups(int n, gid_t* list) { return reterr(sys3(SYS_getgroups, n, (int) list, 0)); }
int setgroups(int n, const gid_t* list) { return reterr(sys3(SYS_setgroups, n, (int) list, 0)); }

/* dprintf/vdprintf: format to a buffer then write() to the fd. picolibc omits these from the
 * freestanding build; toybox's xprintf layer uses dprintf. */
int vdprintf(int fd, const char* fmt, va_list ap) {
	char buf[2048];
	int n = vsnprintf(buf, sizeof buf, fmt, ap);
	if (n < 0) return n;
	if (n > (int) sizeof buf) n = (int) sizeof buf;   // truncated (best effort)
	return write(fd, buf, n);
}
int dprintf(int fd, const char* fmt, ...) {
	va_list ap; va_start(ap, fmt);
	int n = vdprintf(fd, fmt, ap);
	va_end(ap);
	return n;
}
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

/* pread/pwrite: read/write at an explicit offset without a separate lseek visible to the caller.
 * NanOS has no positioned-I/O syscall, so we save the current offset, seek, transfer, and restore
 * it. Not atomic against a concurrent lseek on the same fd, but every NanOS consumer (e.g.
 * darkhttpd, which forks one process per connection) owns its fd exclusively. */
int pread(int fd, void* buf, unsigned n, int off) {
	int cur = sys3(SYS_lseek, fd, 0, 1);                 /* SEEK_CUR */
	if (cur < 0) { errno = -cur; return -1; }
	if (sys3(SYS_lseek, fd, off, 0) < 0) { errno = ESPIPE; return -1; }   /* SEEK_SET */
	int r = sys3(SYS_read, fd, (int) buf, (int) n);
	sys3(SYS_lseek, fd, cur, 0);                         /* restore */
	if (r < 0) { errno = -r; return -1; }
	return r;
}
int pwrite(int fd, const void* buf, unsigned n, int off) {
	int cur = sys3(SYS_lseek, fd, 0, 1);
	if (cur < 0) { errno = -cur; return -1; }
	if (sys3(SYS_lseek, fd, off, 0) < 0) { errno = ESPIPE; return -1; }
	int r = sys3(SYS_write, fd, (int) buf, (int) n);
	sys3(SYS_lseek, fd, cur, 0);
	if (r < 0) { errno = -r; return -1; }
	return r;
}
int unlink(const char* p) {
	return reterr(sys3(SYS_unlink, (int) p, 0, 0));
}
int link(const char* oldp, const char* newp) {
	return reterr(sys3(SYS_link, (int) oldp, (int) newp, 0));
}
int symlink(const char* target, const char* linkpath) {
	return reterr(sys3(SYS_symlink, (int) target, (int) linkpath, 0));
}
int mkdir(const char* p, mode_t mode) {
	return reterr(sys3(SYS_mkdir, (int) p, (int) mode, 0));
}
int rmdir(const char* p)                { return reterr(sys3(SYS_rmdir, (int) p, 0, 0)); }
int creat(const char* p, mode_t m)      { return reterr(sys3(SYS_creat, (int) p, (int) m, 0)); }
int fsync(int fd)                       { return reterr(sys3(SYS_fsync, fd, 0, 0)); }
int fdatasync(int fd)                   { return reterr(sys3(SYS_fdatasync, fd, 0, 0)); }
void sync(void)                         { sys3(SYS_sync, 0, 0, 0); }
int fchdir(int fd)                      { return reterr(sys3(SYS_fchdir, fd, 0, 0)); }
int ftruncate(int fd, off_t length)     { return reterr(sys3(SYS_ftruncate, fd, (int) length, 0)); }
int truncate(const char* p, off_t length) { return reterr(sys3(SYS_truncate, (int) p, (int) length, 0)); }
/* statfs(2)/fstatfs(2): real filesystem stats (free/total blocks). The kernel always fills the
 * Linux i386 layout — sixteen 32-bit words — regardless of arch, so read it into a uint32_t[16]
 * and map onto the (long-field) userspace struct statfs. This is what the file explorer uses for
 * "free space"; it supersedes the canned statvfs() stub for callers that go through statfs(). */
#include <sys/statfs.h>
static int statfs_words(int nr, int a, struct statfs* buf) {
	if (!buf) { errno = EFAULT; return -1; }
	uint32_t k[16];
	int r = reterr(sys3(nr, a, (int) k, 0));
	if (r < 0) return r;
	memset(buf, 0, sizeof *buf);
	buf->f_type    = (long) k[0];
	buf->f_bsize   = (long) k[1];
	buf->f_blocks  = (long) k[2];
	buf->f_bfree   = (long) k[3];
	buf->f_bavail  = (long) k[4];
	buf->f_files   = (long) k[5];
	buf->f_ffree   = (long) k[6];
	buf->f_namelen = (long) k[9];
	buf->f_frsize  = (long) k[10];
	return 0;
}
int statfs(const char* p, struct statfs* buf)  { return statfs_words(SYS_statfs, (int) p, buf); }
int fstatfs(int fd, struct statfs* buf)         { return statfs_words(SYS_fstatfs, fd, buf); }
/* utime(2): the kernel reads struct utimbuf {time_t actime, modtime} directly (NULL -> now). */
int utime(const char* path, const struct utimbuf* times) {
	return reterr(sys3(SYS_utime, (int) path, (int) times, 0));
}
/* Permission ops. The ext FS is read-write, so these call the real syscalls (they were no-op
 * stubs in posixstubs.c back when the disk was read-only). NanOS is single-user/root, but chmod
 * actually changes the on-disk mode and chown the owner, persisting through JBD2. */
int chmod(const char* p, mode_t m)          { return reterr(sys3(SYS_chmod,  (int) p, (int) m, 0)); }
int fchmod(int fd, mode_t m)                { return reterr(sys3(SYS_fchmod, fd, (int) m, 0)); }
int chown(const char* p, uid_t u, gid_t g)  { return reterr(sys3(SYS_chown,  (int) p, (int) u, (int) g)); }
int lchown(const char* p, uid_t u, gid_t g) { return reterr(sys3(SYS_lchown, (int) p, (int) u, (int) g)); }
int fchown(int fd, uid_t u, gid_t g)        { return reterr(sys3(SYS_fchown, fd, (int) u, (int) g)); }
/* rename(2): a real atomic rename via SYS_rename (ext write support added it). This supersedes
 * the old userland copy+unlink stopgap, so directories and hard links keep their inode and mv
 * works for any node type within the filesystem. */
int rename(const char* oldp, const char* newp) {
	return reterr(sys3(SYS_rename, (int) oldp, (int) newp, 0));
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
int killpg(int pgrp, int s)             { return kill(-pgrp, s); }   /* sudo needs killpg */

/* writev: picolibc has no scatter-gather write; emulate over write(). sudo uses it for log output. */
#include <sys/uio.h>
ssize_t writev(int fd, const struct iovec* iov, int n) {
	ssize_t total = 0;
	for (int i = 0; i < n; i++) {
		if (iov[i].iov_len == 0) continue;
		int r = write(fd, iov[i].iov_base, (int) iov[i].iov_len);
		if (r < 0) return total ? total : r;
		total += r;
		if (r < (int) iov[i].iov_len) break;   // short write
	}
	return total;
}
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

/* sigaction(2): routed through SYS_rt_sigaction with the kernel-ABI struct k_sigaction (the
 * same layout musl marshals into), carrying an 8-byte sigsetsize. picolibc's sigset_t is a
 * single 32-bit word and apps only use signals 1..31, so we don't copy sa_mask into the
 * 64-bit kernel mask — the kernel ignores it anyway (it implies SA_RESTART and runs the
 * handler with just the delivered signal blocked). We supply the libc sigreturn trampoline
 * as sa_restorer. act == NULL queries without changing; the previous handler comes back in
 * `old`. */
int sigaction(int sig, const struct sigaction* act, struct sigaction* old) {
	struct k_sigaction ka, ko;
	memset(&ka, 0, sizeof ka);
	memset(&ko, 0, sizeof ko);
	if (act) {
		ka.k_sa_handler  = (void*) (size_t) act->sa_handler;
		ka.k_sa_flags    = (unsigned long) act->sa_flags;
		ka.k_sa_restorer = (void*) &__nx_sigtramp;
		/* sa_mask intentionally left zero — see comment above. */
	}
	int r = sys4(SYS_rt_sigaction, sig, act ? (int) &ka : 0, old ? (int) &ko : 0, 8);
	if (r < 0) { errno = -r; return -1; }
	if (old) {
		old->sa_handler = (void (*)(int)) (size_t) ko.k_sa_handler;
		old->sa_flags = 0;
		memset(&old->sa_mask, 0, sizeof old->sa_mask);
	}
	return 0;
}

/* sigprocmask(2): routed through SYS_rt_sigprocmask, which carries the mask by pointer + an
 * 8-byte sigsetsize (so signals 32..64 are addressable). picolibc's sigset_t is one 32-bit
 * word and apps only touch signals 1..31, so we zero-extend it into a local 64-bit mask for
 * the call and narrow the result back. picolibc uses BSD `how` values (SETMASK=0, BLOCK=1,
 * UNBLOCK=2) but the kernel uses the Linux numbering (BLOCK=0, UNBLOCK=1, SETMASK=2), so
 * translate. A NULL set queries without changing. */
int sigprocmask(int how, const sigset_t* set, sigset_t* old) {
	int khow = set ? (how == SIG_BLOCK ? 0 : how == SIG_UNBLOCK ? 1 : 2) : 0;
	uint64_t kset = set ? (uint64_t) (unsigned) *set : 0;
	uint64_t kold = 0;
	int r = sys4(SYS_rt_sigprocmask, khow, set ? (int) &kset : 0, (int) &kold, 8);
	if (r < 0) { errno = -r; return -1; }
	if (old) *old = (sigset_t) (unsigned) kold;
	return 0;
}

/* pause(2): block until a signal handler runs. Always returns -1 with errno == EINTR (the
 * kernel never restarts pause). */
int pause(void) {
	return reterr(sys3(SYS_pause, 0, 0, 0));
}

/* sigsuspend(2): install *mask as the blocked set, wait for a deliverable signal, restore the
 * previous mask. Routed through SYS_rt_sigsuspend (mask by pointer + 8-byte sigsetsize); the
 * single-word picolibc sigset_t is zero-extended into a local 64-bit mask. Always returns -1
 * with errno == EINTR. */
int sigsuspend(const sigset_t* mask) {
	uint64_t kmask = mask ? (uint64_t) (unsigned) *mask : 0;
	return reterr(sys3(SYS_rt_sigsuspend, mask ? (int) &kmask : 0, 8, 0));
}

/* sigpending(2): the set of signals pending (but blocked) for the caller. Routed through
 * SYS_rt_sigpending (mask by pointer + 8-byte sigsetsize). picolibc's sigset_t is a single
 * 32-bit word and apps only inspect signals 1..31, so the 64-bit kernel mask is narrowed
 * back into it. NULL set is a no-op success. (Previously undefined -> resolved-to-0; an app
 * like vim that references &sigpending then crashed.) */
int sigpending(sigset_t* set) {
	uint64_t kset = 0;
	int r = sys3(SYS_rt_sigpending, (int) &kset, 8, 0);
	if (r < 0) { errno = -r; return -1; }
	if (set) *set = (sigset_t) (unsigned) kset;
	return 0;
}

/* setitimer(2)/getitimer(2): only ITIMER_REAL is implemented in the kernel — a per-process
 * wall-clock timer that fires SIGALRM and re-arms from the interval. The platform
 * `struct itimerval` is marshalled into the kernel-ABI `struct k_itimerval` (same pattern as
 * sigaction/k_sigaction), so the kernel never depends on the libc timeval layout. ITIMER_REAL
 * is what alarm() and editors' (vim's) terminal-response timeouts need. */
static void itv_to_kernel(const struct itimerval* s, struct k_itimerval* d) {
	d->it_interval_sec  = (long) s->it_interval.tv_sec;
	d->it_interval_usec = (long) s->it_interval.tv_usec;
	d->it_value_sec     = (long) s->it_value.tv_sec;
	d->it_value_usec    = (long) s->it_value.tv_usec;
}
static void itv_from_kernel(struct itimerval* d, const struct k_itimerval* s) {
	d->it_interval.tv_sec  = (time_t) s->it_interval_sec;
	d->it_interval.tv_usec = (suseconds_t) s->it_interval_usec;
	d->it_value.tv_sec     = (time_t) s->it_value_sec;
	d->it_value.tv_usec    = (suseconds_t) s->it_value_usec;
}
int setitimer(int which, const struct itimerval* nval, struct itimerval* oval) {
	struct k_itimerval kn, ko;
	if (nval) itv_to_kernel(nval, &kn);
	int r = sys3(SYS_setitimer, which, nval ? (int) &kn : 0, oval ? (int) &ko : 0);
	if (r < 0) { errno = -r; return -1; }
	if (oval) itv_from_kernel(oval, &ko);
	return 0;
}
int getitimer(int which, struct itimerval* oval) {
	struct k_itimerval ko;
	int r = sys3(SYS_getitimer, which, oval ? (int) &ko : 0, 0);
	if (r < 0) { errno = -r; return -1; }
	if (oval) itv_from_kernel(oval, &ko);
	return 0;
}

/* getrandom(2): fill buf with CSPRNG bytes from the kernel (kernel/Csprng.*, seeded at boot from
 * RDRAND+RDTSC-jitter+RTC). The kernel never blocks and ignores the flags (always seeded), so it
 * returns the full count; we surface that count like Linux. This is the seam OpenSSL/picolibc use
 * for real entropy. */
ssize_t getrandom(void* buf, size_t n, unsigned flags) {
	return (ssize_t) reterr(sys3(SYS_getrandom, (int) buf, (int) n, (int) flags));
}

/* getentropy(3): the cryptographic entropy primitive (picolibc's arc4random + any ported crypto
 * library seed from it). Backed by the real kernel CSPRNG via getrandom — NO fixed-seed fallback,
 * so the bytes differ every boot. Caps at 256 bytes and is all-or-nothing, per POSIX. */
int getentropy(void* buf, size_t n) {
	if (n > 256) { errno = EIO; return -1; }   /* getentropy caps at 256 bytes */
	ssize_t r = getrandom(buf, n, 0);
	if (r < 0) return -1;                       /* errno set by getrandom */
	if ((size_t) r != n) { errno = EIO; return -1; }
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
	int r = sys3(SYS_execve, (int) path, (int) argv, (int) envp);
	/* NanOS programs are <name>.nxe. A caller that execs a bare name (e.g. git's run-command
	 * spawning "git-pack-objects" from GIT_EXEC_PATH) gets -ENOENT for the extensionless path;
	 * retry once with ".nxe" appended so the .nxe convention is transparent to ported software.
	 * bash already appends .nxe itself, so its execs hit the first attempt — this only adds the
	 * fallback for programs that don't know about the extension. */
	if (r == -2 && path) {                 /* -ENOENT */
		size_t n = 0; while (path[n]) n++;
		int hasNxe = n >= 4 && path[n-4] == '.' && path[n-3] == 'n' && path[n-2] == 'x' && path[n-1] == 'e';
		if (!hasNxe && n + 5 <= 512) {
			char buf[512];
			for (size_t i = 0; i < n; i++) buf[i] = path[i];
			buf[n] = '.'; buf[n+1] = 'n'; buf[n+2] = 'x'; buf[n+3] = 'e'; buf[n+4] = 0;
			r = sys3(SYS_execve, (int) buf, (int) argv, (int) envp);
		}
	}
	return reterr(r);
}

/* execv(3): exec with the caller's current environment (picolibc ships only execve). inetd's
 * service launcher (execv(se_server, se_argv)) goes through here. */
int execv(const char* path, char* const argv[]) {
	return execve(path, argv, environ);
}

/* execl(3): variadic form — collect the NULL-terminated arg list into an argv[] and execv it.
 * Used by the telnet client's shell-escape. Capped at 63 args + the terminator. */
int execl(const char* path, const char* arg0, ...) {
	char* argv[64];
	int n = 0;
	argv[n++] = (char*) arg0;
	va_list ap; va_start(ap, arg0);
	while (n < 63) {
		char* a = va_arg(ap, char*);
		argv[n++] = a;
		if (!a) break;
	}
	va_end(ap);
	argv[n] = 0;
	return execve(path, argv, environ);
}

/* Console input mode: 0 = cooked (line-edited), 1 = raw (per-key). The shell uses
 * raw for its own line editor and cooked while a child program runs. */
int termmode(int raw) {
	return sys3(SYS_termmode, raw, 0, 0);
}

/* DIAG (Dell GL-desktop freeze) — LOSSLESS, remove once root-caused. The freeze ends in a reboot
 * that swallows the buffered stdout->pipe->nwm.txt tail, and a kernel-side ioctl log made it worse
 * (a synchronous USB write from the compositor's ioctl context, ~10^2 GEM calls/frame). So log from
 * the calling process straight to a file with a synchronous ext write() BEFORE and AFTER the truly
 * BLOCKING DRM ioctls: EXECBUFFER2 (0x69 submission), GEM_WAIT (0x6c), SYNCOBJ_WAIT (0xca),
 * TIMELINE_WAIT (0xcf). GEM_SET_DOMAIN (0x4b) is deliberately NOT logged — it is high-volume upload
 * traffic and Dell boot #53 proved it is not the stall (frames 0-4 finished their uploads). A
 * trailing "enter" with no matching "ret" is the hang site; all "enter"+"ret" then silence = the
 * stall is elsewhere in Mesa. DRM ioctls carry _IOC type 'd'; gate on that. */
#include <fcntl.h>
static void drm_ioctl_diag(const char *buf, int n) {
	static int fd = -2;
	if (fd == -2)
		fd = open("/disks/main/nanos/logs/gldiag.txt", O_WRONLY | O_CREAT | O_APPEND, 0666);
	if (fd < 0 || n <= 0)
		return;
	write(fd, buf, n);
	fsync(fd);   /* no-op today (ext writes flush synchronously), correct-intent for the future */
}

/* ioctl(2): used for the framebuffer (FBIOGET_*SCREENINFO). The single pointer arg is
 * passed through to the device. */
int ioctl(int fd, unsigned long request, ...) {
	va_list ap;
	va_start(ap, request);
	void* arg = va_arg(ap, void*);
	va_end(ap);
	unsigned type = (unsigned) (request >> 8) & 0xffu;
	unsigned nr   = (unsigned) request & 0xffu;
	/* Freeze localization, THROTTLED. The full every-ioctl trace served its purpose (it proved the
	 * trailing op differs per freeze), but it costs TWO synchronous ext->USB writes per DRM ioctl
	 * from the compositor's hot loop — tens per frame, a growing file, real milliseconds each on
	 * the Dell's per-sector USB path. That write storm dominated frame times and is itself a
	 * suspect in the wedge it was hunting. Steady-state "which ioctl is in flight / hung" is now
	 * answered KERNEL-SIDE by the i915 pulse thread (2 s flight recorder: enters/exits + last
	 * nr@pid, persisted to pulse.txt) — so the hot loop pays NOTHING here:
	 *   - the first FULL_TRACE DRM ioctls log ENTER+ret unconditionally (covers session startup);
	 *   - afterwards a DRM ioctl logs only when it FAILS unexpectedly (r<0, excluding the
	 *     poll-idiom SYNCOBJ waits whose ETIME misses are routine and high-volume). */
	static int diag_full_left = 300;
	int diag = 0, ret;
	char b[64];
	if (type == 'd' && diag_full_left > 0) { diag = 1; diag_full_left--; }
	if (diag)
		drm_ioctl_diag(b, snprintf(b, sizeof b, "drm ioctl ENTER nr=0x%x\n", nr));
	ret = reterr(sys3(SYS_ioctl, fd, (int) request, (int) arg));
	if (diag)
		drm_ioctl_diag(b, snprintf(b, sizeof b, "drm ioctl ret   nr=0x%x r=%d\n", nr, ret));
	else if (type == 'd' && ret < 0 && !(nr == 0xc3 || nr == 0xca || nr == 0xcf))
		drm_ioctl_diag(b, snprintf(b, sizeof b, "drm ioctl FAIL  nr=0x%x r=%d\n", nr, ret));
	return ret;
}

/* 5-argument syscall (i386: ebx/ecx/edx/esi/edi; x86_64: rdi/rsi/rdx/r10/r8) for mmap, which
 * needs more than three args. */
static inline int sys5(int nr, int a, int b, int c, int d, int e) {
	int r;
#if defined(__x86_64__)
	long rr;
	register long r10 __asm__("r10") = (long) d;
	register long r8  __asm__("r8")  = (long) e;
	__asm__ __volatile__("syscall" : "=a"(rr)
		: "a"((long) nr), "D"((long) a), "S"((long) b), "d"((long) c), "r"(r10), "r"(r8)
		: "rcx", "r11", "memory");
	r = (int) rr;
#else
	__asm__ __volatile__("int $0x80"
		: "=a"(r) : "a"(nr), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e) : "memory");
#endif
	return r;
}

/* 5-arg syscall with a FULL 64-bit final argument (x86_64 r8). mmap needs this: DRM GEM fake
 * mmap offsets are >= 0x100000000 (the DRM vma manager allocates from DRM_FILE_PAGE_OFFSET_START
 * = 1<<20 pages), so casting the offset to int (the old bug) stripped the high bits and the
 * kernel's drm_vma_offset lookup missed — the mapping silently fell back to anonymous zero pages,
 * so guest GPU-buffer writes never reached the resource backing (virgl draws rendered nothing). */
static inline long sys_mmap(int nr, long a, long b, long c, long d, long e) {
#if defined(__x86_64__)
	long rr;
	register long r10 __asm__("r10") = d;
	register long r8  __asm__("r8")  = e;
	__asm__ __volatile__("syscall" : "=a"(rr)
		: "a"((long) nr), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
		: "rcx", "r11", "memory");
	return rr;
#else
	int r;
	__asm__ __volatile__("int $0x80"
		: "=a"(r) : "a"(nr), "b"((int)a), "c"((int)b), "d"((int)c), "S"((int)d), "D"((int)e) : "memory");
	return r;
#endif
}

/* mmap(2): pass length/prot/flags/fd/offset to the kernel. Supports device mappings
 * (e.g. /dev/fb0, DRM GEM BOs), anonymous mappings (fd < 0), and file-backed mappings
 * (regular-file fd, eagerly loaded). `addr` is advisory and ignored (the kernel picks the VA).
 * The offset is passed 64-bit-wide (see sys_mmap). Returns MAP_FAILED ((void*)-1) on error. */
void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
	(void) addr; (void) flags;
	long r = sys_mmap(SYS_mmap2, (long) length, prot, flags, fd, (long) offset);
	if (r < 0 && r >= -4095) { errno = (int) -r; return (void*) -1; }
	return (void*) r;
}

/* munmap(2): issue SYS_munmap so the kernel clears the PTEs, frees the backing frames, and
 * records the VA range for reuse by a later mmap (the mmap window is finite — 64 MiB — so
 * VA reclaim is what lets long-lived thread create/join churn keep recycling stacks). The
 * kernel only acts on ranges inside its anonymous mmap window; anything else is a no-op. */
int munmap(void* addr, size_t length) {
	int r = sys3(SYS_munmap, (int) addr, (int) length, 0);
	if (r < 0) { errno = -r; return -1; }
	return 0;
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
/* MUST match the kernel's LinuxStat (kernel/Syscall.h): the kernel writes this exact 48-byte
 * record into the user buffer for stat/lstat/fstat/fstatat. It was widened to 64-bit fields
 * for the x86_64 layout (commit "LinuxStat widened to the x86_64 layout"); a stale 28-byte
 * struct here both mis-maps the fields AND overflows the caller's stack buffer (corrupting the
 * saved callee-saved registers — e.g. ls's mkent keeps &entry in %rbx across stat, so the
 * overflow faulted it). Fixed-width types => identical layout on i386 and x86_64. */
/* Plain field names (NOT st_*): st_mtime/st_atime/... are POSIX macros (st_mtim.tv_sec) in
 * picolibc, so they cannot be struct member names — only the layout has to match. */
struct knl_stat {
	uint64_t ino;
	uint32_t mode;
	uint32_t nlink;
	uint32_t uid;
	uint32_t gid;
	uint64_t size;
	uint64_t blocks;
	int64_t  mtime;
};
static void fillstat(struct stat* o, const struct knl_stat* k) {
	memset(o, 0, sizeof *o);
	o->st_mode = k->mode;
	o->st_size = k->size;
	o->st_nlink = k->nlink;
	o->st_uid = k->uid;
	o->st_gid = k->gid;
	o->st_mtime = k->mtime;
	o->st_ino = k->ino;
	o->st_blocks = k->blocks;
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

/* ---- the *at family + utimes/utimensat ----
 * Directory-relative ops (dirfd == AT_FDCWD resolves against the cwd in the kernel). sbase's
 * libutil/recurse.c traverses with openat/fstatat/unlinkat/readlinkat, and touch sets times via
 * utimensat. All kernel-side (kernel/Syscall.cpp); register order is a0=ebx..a4=edi. Returns are
 * `int` to match this file's convention — on i386 int/ssize_t/mode_t/uid_t are all 32-bit, so the
 * ABI matches picolibc's POSIX prototypes that sbase compiles against. No <fcntl.h>/<unistd.h>
 * include here on purpose (they'd clash with the int read/write decls above). */
int openat(int dirfd, const char* p, int flags, ...) {
	return reterr(sys3(SYS_openat, dirfd, (int) p, flags));   /* kernel openat ignores mode */
}
int mkdirat(int dirfd, const char* p, mode_t m)   { return reterr(sys3(SYS_mkdirat, dirfd, (int) p, (int) m)); }
int unlinkat(int dirfd, const char* p, int flags) { return reterr(sys3(SYS_unlinkat, dirfd, (int) p, flags)); }
int renameat(int ofd, const char* op, int nfd, const char* np) {
	return reterr(sys4(SYS_renameat, ofd, (int) op, nfd, (int) np));
}
int linkat(int ofd, const char* op, int nfd, const char* np, int flags) {
	return reterr(sys5(SYS_linkat, ofd, (int) op, nfd, (int) np, flags));
}
int symlinkat(const char* target, int nfd, const char* p) {
	return reterr(sys3(SYS_symlinkat, (int) target, nfd, (int) p));
}
int readlinkat(int dirfd, const char* p, char* buf, size_t n) {
	return reterr(sys4(SYS_readlinkat, dirfd, (int) p, (int) buf, (int) n));
}
int fchmodat(int dirfd, const char* p, mode_t m, int flags) {
	return reterr(sys4(SYS_fchmodat, dirfd, (int) p, (int) m, flags));
}
int fchownat(int dirfd, const char* p, uid_t u, gid_t g, int flags) {
	return reterr(sys5(SYS_fchownat, dirfd, (int) p, (int) u, (int) g, flags));
}
int faccessat(int dirfd, const char* p, int mode, int flags) {
	return reterr(sys4(SYS_faccessat, dirfd, (int) p, mode, flags));
}
int fstatat(int dirfd, const char* p, struct stat* o, int flags) {
	struct knl_stat k;
	int r = sys4(SYS_fstatat64, dirfd, (int) p, (int) &k, flags);
	if (r < 0) { errno = -r; return -1; }
	fillstat(o, &k);
	return 0;
}
/* utimensat: the kernel reads struct timespec[2] (or NULL=now) directly, honoring UTIME_NOW/OMIT
 * in tv_nsec — pass the pointer straight through (i386 timespec = {long long sec @0; long ns @8}
 * = 12 bytes, exactly what the kernel decodes). */
int utimensat(int dirfd, const char* p, const struct timespec times[2], int flags) {
	return reterr(sys4(SYS_utimensat, dirfd, (int) p, (int) times, flags));
}
/* futimens(fd, times): set the open file's times. Like glibc, this is utimensat with an empty
 * path on the fd itself (the kernel's AT_EMPTY_PATH handling in resolveAt). */
int futimens(int fd, const struct timespec times[2]) {
	static const char empty[1] = { 0 };
	return reterr(sys4(SYS_utimensat, fd, (int) empty, (int) times, 0));
}
/* utimes(timeval[2]): the kernel's utimes takes whole seconds (not a pointer), so extract tv_sec
 * here; NULL -> now via time(). */
int utimes(const char* p, const struct timeval times[2]) {
	unsigned at, mt;
	if (!times) { at = mt = (unsigned) time(0); }
	else { at = (unsigned) times[0].tv_sec; mt = (unsigned) times[1].tv_sec; }
	return reterr(sys3(SYS_utimes, (int) p, (int) at, (int) mt));
}

/* The picolibc retargetable locks (__retarget_lock_*) are implemented over the kernel
 * futex in user/libc-glue/retarget_lock.c, not here. */

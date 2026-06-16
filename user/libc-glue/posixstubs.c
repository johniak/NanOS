/*
 * posixstubs.c — POSIX functions bash (and other ports) reference that picolibc does not
 * provide. NanOS is single-user (everything is root) with a read-only disk + /tmp, so the
 * identity/permission calls are honest no-ops and the user/group database resolves only
 * root. These live in libc.ndl alongside the syscall glue.
 */
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <signal.h>
#include <pwd.h>
#include <grp.h>
#include <stdarg.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>

extern char** environ;

/* Pseudo-terminal helpers (openpty/forkpty/login_tty) now have a real implementation in pty.c,
 * backed by the kernel /dev/ptmx + /dev/pts0 pair. */

/* pathconf/fpathconf: report fixed POSIX limits (NanOS has no per-path configuration). Values
 * match <limits.h> (PATH_MAX 4096, NAME_MAX 255). Without this, gnulib/wget fall back to a
 * raw PATH_MAX that isn't always in scope. */
long pathconf(const char* path, int name) {
	(void) path;
	switch (name) {
	case _PC_LINK_MAX:    return 127;
	case _PC_MAX_CANON:   return 255;
	case _PC_MAX_INPUT:   return 255;
	case _PC_NAME_MAX:    return 255;
	case _PC_PATH_MAX:    return 4096;
	case _PC_PIPE_BUF:    return 4096;
	case _PC_CHOWN_RESTRICTED: return 1;
	case _PC_NO_TRUNC:    return 1;
	case _PC_VDISABLE:    return 0;
	case _PC_SYMLINK_MAX: return 4096;
	case _PC_2_SYMLINKS:  return 1;
	case _PC_FILESIZEBITS: return 32;
	default:              return -1;
	}
}
long fpathconf(int fd, int name) { (void) fd; return pathconf("/", name); }

/* uname: report a fixed NanOS identity (no per-host config). */
int uname(struct utsname* buf) {
	if (!buf) { errno = EFAULT; return -1; }
	strcpy(buf->sysname, "NanOS");
	strcpy(buf->nodename, "nanos");
	strcpy(buf->release, "1.0");
	strcpy(buf->version, "NanOS 1.0");
	strcpy(buf->machine, "i686");
	strcpy(buf->domainname, "(none)");
	return 0;
}

/* ---- identity: single-user root ---- */
uid_t getuid(void)   { return 0; }
uid_t geteuid(void)  { return 0; }
gid_t getgid(void)   { return 0; }
gid_t getegid(void)  { return 0; }
int setuid(uid_t u)  { (void) u; return 0; }
int seteuid(uid_t u) { (void) u; return 0; }
int setgid(gid_t g)  { (void) g; return 0; }
int setegid(gid_t g) { (void) g; return 0; }
int setreuid(uid_t r, uid_t e) { (void) r; (void) e; return 0; }
int setregid(gid_t r, gid_t e) { (void) r; (void) e; return 0; }
int getgroups(int n, gid_t* list) { (void) n; (void) list; return 0; }
int setgroups(int n, const gid_t* list) { (void) n; (void) list; return 0; }
/* initgroups: single-user NanOS has no supplementary-group database; a no-op succeed (sshd/login
 * call it when dropping into a session). */
int initgroups(const char* user, gid_t group) { (void) user; (void) group; return 0; }

/* ---- permission ops ---- chmod/fchmod/chown/lchown/fchown now call the real syscalls (the ext
 * FS is read-write); their implementations live in syscalls.c beside the other file-metadata ops.
 * They used to be no-ops here when the disk was read-only. */

/* chroot: NanOS has no per-process root. Reported as unsupported; callers (darkhttpd) only invoke
 * it when explicitly asked to (--chroot), which we never do — the symbol just needs to resolve. */
int chroot(const char* path) { (void) path; errno = ENOSYS; return -1; }

/* getrusage: no per-process resource accounting. Zero the struct and succeed (servers query it for
 * optional stats logging; zeros are an honest "not measured"). */
int getrusage(int who, struct rusage* usage) {
	(void) who;
	if (usage) memset(usage, 0, sizeof *usage);
	return 0;
}

/* umask: track the value so callers round-trip it (the kernel does not apply it yet). */
static mode_t g_umask = 022;
mode_t umask(mode_t m) { mode_t o = g_umask; g_umask = m & 0777; return o; }

/* access(2): we have no permission model, so existence (via stat) decides; mode is ignored
 * beyond that (everything is accessible to root). */
int access(const char* path, int mode) {
	(void) mode;
	struct stat st;
	return stat(path, &st) == 0 ? 0 : -1;
}

/* getprogname/setprogname (BSD): the running program's short name. crt0 seeds it from argv[0]
 * via __nx_set_progname; gnulib's error() and many coreutils use getprogname() for the message
 * prefix. Kept tiny — just a pointer to the basename of whatever was set. */
static const char* g_progname = "nanos";
static const char* nx_basename(const char* p) {
	const char* b = p;
	for (; p && *p; p++)
		if (*p == '/') b = p + 1;
	return b;
}
const char* getprogname(void) { return g_progname; }
void setprogname(const char* p) { if (p && *p) g_progname = nx_basename(p); }
void __nx_set_progname(const char* argv0) { if (argv0 && *argv0) g_progname = nx_basename(argv0); }

/* __fpending (glibc <stdio_ext.h>): bytes still in a stream's output buffer. gnulib's closeout
 * uses it to detect a write error at exit. picolibc's tinystdio writes through without an
 * exposed buffer, so report 0 (nothing pending) — closeout then relies on the close/fflush
 * return value, which is correct here. */
#include <stdio.h>
size_t __fpending(FILE* fp) { (void) fp; return 0; }

/* getrlimit/setrlimit: NanOS has a single flat address space and no per-process limits, so
 * every resource is reported as unlimited and setting one is accepted-and-ignored. Ports probe
 * these (vim sizes its memory off RLIMIT_DATA); "no limit" is the honest answer. */
int getrlimit(int resource, struct rlimit* rl) {
	(void) resource;
	if (rl) { rl->rlim_cur = RLIM_INFINITY; rl->rlim_max = RLIM_INFINITY; }
	return 0;
}
int setrlimit(int resource, const struct rlimit* rl) { (void) resource; (void) rl; return 0; }

/* sigaltstack: no alternate signal stack (handlers run on the normal stack). Report "disabled"
 * and accept any request, so crash-handler setup (vim, bash) succeeds as a no-op. */
int sigaltstack(const stack_t* ss, stack_t* old) {
	(void) ss;
	if (old) { old->ss_sp = 0; old->ss_size = 0; old->ss_flags = SS_DISABLE; }
	return 0;
}

/* execvp(3): picolibc provides only execve. Search PATH for a bare name (no '/'); a name with a
 * slash is exec'd as-is. execve only returns on failure, so we keep trying entries. */
int execvp(const char* file, char* const argv[]) {
	if (!file || !*file) { errno = ENOENT; return -1; }
	if (strchr(file, '/')) return execve(file, argv, environ);
	const char* path = getenv("PATH");
	if (!path || !*path) path = "/disks/main/nanos/bin:/disks/main/bin";
	char buf[512];
	for (const char* p = path; ; ) {
		const char* colon = strchr(p, ':');
		size_t len = colon ? (size_t) (colon - p) : strlen(p);
		if (len && len + 1 + strlen(file) + 1 <= sizeof buf) {
			memcpy(buf, p, len);
			buf[len] = '/';
			strcpy(buf + len + 1, file);
			execve(buf, argv, environ);   /* returns only on failure */
		}
		if (!colon) break;
		p = colon + 1;
	}
	errno = ENOENT;
	return -1;
}

/* execlp(3): variadic front-end to execvp(3). Collect the NULL-terminated argument list into a
 * vector on the stack, then delegate (PATH search + execve). Used by `git help` to launch a man/
 * info viewer; picolibc ships neither execlp nor execvp. */
int execlp(const char* file, const char* arg0, ...) {
	char* argv[64];
	int n = 0;
	argv[n++] = (char*) arg0;
	va_list ap;
	va_start(ap, arg0);
	while (n < (int) (sizeof argv / sizeof argv[0]) - 1) {
		char* a = va_arg(ap, char*);
		argv[n++] = a;
		if (!a) break;
	}
	va_end(ap);
	argv[n] = 0;   /* guarantee NULL termination if the list overflowed */
	return execvp(file, argv);
}

/* pthread_sigmask(3): NanOS keeps a single process-wide signal mask (one kernel mask per
 * process), so the per-thread mask is the process mask — delegate to sigprocmask(2). git's
 * run-command.c blocks all signals around fork() and restores afterward through this call. */
int pthread_sigmask(int how, const sigset_t* set, sigset_t* old) {
	return sigprocmask(how, set, old);
}

/* statvfs(3)/fstatvfs(3): NanOS has no statvfs syscall. The only caller is git's diagnostic
 * dump (`git bugreport`/`git diagnose` -> compat/disk.h get_disk_info), which just prints the
 * numbers — never on a hot path. Report a plausible, consistent filesystem so the call
 * succeeds rather than erroring. */
int statvfs(const char* path, struct statvfs* buf) {
	(void) path;
	if (!buf) { errno = EFAULT; return -1; }
	memset(buf, 0, sizeof *buf);
	buf->f_bsize  = 4096;
	buf->f_frsize = 4096;
	buf->f_namemax = 255;
	return 0;
}
int fstatvfs(int fd, struct statvfs* buf) {
	(void) fd;
	return statvfs("/", buf);
}

/* mkdtemp(3): create a uniquely-named directory from a "...XXXXXX" template. picolibc declares
 * it but does not implement it. Derive the suffix from pid + an attempt counter and mkdir until
 * one sticks (no atomic O_EXCL dir create, but EEXIST retry is sufficient on a single user). */
char* mkdtemp(char* tmpl) {
	size_t len = tmpl ? strlen(tmpl) : 0;
	if (len < 6 || strcmp(tmpl + len - 6, "XXXXXX") != 0) { errno = EINVAL; return 0; }
	static const char cs[] = "abcdefghijklmnopqrstuvwxyz0123456789";
	unsigned seed = (unsigned) getpid() * 2654435761u;
	for (int attempt = 0; attempt < 256; attempt++) {
		unsigned v = seed + (unsigned) attempt * 40503u;
		for (int i = 0; i < 6; i++) { tmpl[len - 6 + i] = cs[v % 36]; v /= 36; v += attempt; }
		if (mkdir(tmpl, 0700) == 0) return tmpl;
		if (errno != EEXIST) return 0;
	}
	errno = EEXIST;
	return 0;
}

/* alarm/sleep: no SIGALRM timer, so alarm is a no-op (returns 0 = none pending). sleep and
 * usleep block via nanosleep (declared in <time.h> through the glue). */
unsigned alarm(unsigned sec) { (void) sec; return 0; }
unsigned sleep(unsigned sec) {
	struct timespec ts; ts.tv_sec = (time_t) sec; ts.tv_nsec = 0;
	nanosleep(&ts, 0);
	return 0;
}
int usleep(useconds_t usec) {
	struct timespec ts; ts.tv_sec = usec / 1000000; ts.tv_nsec = (long) ((usec % 1000000) * 1000);
	return nanosleep(&ts, 0);
}

/* gethostname/ttyname: a fixed identity. */
int gethostname(char* name, size_t len) {
	const char* h = "nanos";
	if (!name || len == 0) return -1;
	strncpy(name, h, len);
	name[len - 1] = 0;
	return 0;
}
char* ttyname(int fd) { return isatty(fd) ? (char*) "/dev/tty" : 0; }
int ttyname_r(int fd, char* buf, size_t len) {
	if (!isatty(fd)) return ENOTTY;
	strncpy(buf, "/dev/tty", len); buf[len ? len - 1 : 0] = 0;
	return 0;
}

/* user/group database: getpwuid/getpwnam/getgrgid live in pwd_grp.c (they read /etc/passwd);
 * here are the thin remainders. getgrnam delegates to getgrgid so the root entry has one
 * source of truth; enumeration is empty (only root exists). */
struct group* getgrgid(gid_t);
struct passwd* getpwent(void) { return 0; }
void setpwent(void) {}
void endpwent(void) {}
struct group* getgrnam(const char* name) {
	if (!name || strcmp(name, "root") != 0) return 0;
	return getgrgid(0);
}
struct group* getgrent(void) { return 0; }
void setgrent(void) {}
void endgrent(void) {}
char* getlogin(void) { return (char*) "root"; }

/* sysconf: the few values shells query. */
long sysconf(int name) {
	switch (name) {
	case 2:  return 100;    /* _SC_CLK_TCK   (picolibc value) */
	case 4:  return 128;    /* _SC_OPEN_MAX  (our MAXFD) */
	case 8:  return 4096;   /* _SC_PAGESIZE / _SC_PAGE_SIZE */
	default: return -1;
	}
}

/* ---- syslog: route to stderr (NanOS has no syslogd; ports like busybox udhcp expect it) ---- */
#include <stdio.h>
#include <stdarg.h>
static int g_logmask = 0xff;
void openlog(const char* ident, int option, int facility) { (void)ident; (void)option; (void)facility; }
void closelog(void) {}
int  setlogmask(int mask) { int o = g_logmask; if (mask) g_logmask = mask; return o; }
void vsyslog(int priority, const char* fmt, va_list ap) {
	if (!((1 << (priority & 7)) & g_logmask)) return;
	vfprintf(stderr, fmt, ap); fputc('\n', stderr);
}
void syslog(int priority, const char* fmt, ...) {
	va_list ap; va_start(ap, fmt); vsyslog(priority, fmt, ap); va_end(ap);
}

/* clearenv — empty the environment (busybox setup_environment). */
int clearenv(void) { if (environ) environ[0] = 0; return 0; }

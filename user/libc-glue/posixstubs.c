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
#include <limits.h>   /* PATH_MAX for realpath() */
#include <time.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <signal.h>
#include <pwd.h>
#include <grp.h>
#include <stdarg.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/time.h>   /* setitimer/getitimer: alarm() is implemented over ITIMER_REAL */

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

/* ---- identity ---- the real getuid/setuid/... credential wrappers live in syscalls.c (they
 * issue the actual SYS_* calls so userland sees and changes the kernel's per-process Cred).
 * initgroups + getgrouplist live in grp_shadow.c (they consult /etc/group). They used to be
 * single-user-root stubs here when NanOS had no credential model. */

/* ---- permission ops ---- chmod/fchmod/chown/lchown/fchown now call the real syscalls (the ext
 * FS is read-write); their implementations live in syscalls.c beside the other file-metadata ops.
 * They used to be no-ops here when the disk was read-only. */

/* chroot: NanOS has no per-process root. Reported as unsupported; callers (darkhttpd) only invoke
 * it when explicitly asked to (--chroot), which we never do — the symbol just needs to resolve. */
int chroot(const char* path) { (void) path; errno = ENOSYS; return -1; }

/* mknod: NanOS has no mknod syscall (device/fifo nodes are not user-creatable). Only `cp -a` of a
 * block/char/socket/fifo special file reaches this; ordinary file/dir copies never do. Report
 * ENOSYS so the symbol resolves and such a copy fails cleanly rather than silently. */
int mknod(const char* path, mode_t mode, dev_t dev) { (void) path; (void) mode; (void) dev; errno = ENOSYS; return -1; }

/* mkfifoat: same story as mknod — no FIFO filesystem nodes. Mesa's intel_measure profiler
 * (INTEL_MEASURE=control=<path>) is the only caller; it aborts loudly on this error, which is
 * the honest outcome for a genuinely unsupported feature. Never reached in normal rendering. */
int mkfifoat(int dirfd, const char* path, mode_t mode) { (void) dirfd; (void) path; (void) mode; errno = ENOSYS; return -1; }

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

/* getpriority/setpriority: NanOS's scheduler has no per-process nice level, so every process
 * reports the normal priority (0) and a renice is accepted-and-ignored. htop reads/sets these
 * for its NICE column + the F7/F8 renice keys; "normal, can't change" is the honest answer. */
int getpriority(int which, int who) { (void) which; (void) who; return 0; }
int setpriority(int which, int who, int prio) { (void) which; (void) who; (void) prio; return 0; }

/* syscall(): NanOS has no Linux-style numeric syscall multiplexer in userland — syscalls are
 * exposed as named libc functions (libc.ndl imports). Ports that call syscall() directly (htop's
 * capget capability probe) get -ENOSYS; those paths are not reached at runtime here (every NanOS
 * process runs as root, so htop never probes capabilities). */
long syscall(long number, ...) { (void) number; errno = ENOSYS; return -1; }

/* realpath: canonicalize PATH lexically (make absolute via getcwd if relative, then collapse
 * ".", ".." and duplicate slashes) and confirm it exists via stat(). Intermediate symlinks are
 * not expanded here — NanOS's VFS resolves symlink components at access time, and realpath()'s
 * callers (htop canonicalising its htoprc path) only need a canonical, existence-checked path.
 * Writes into RESOLVED (must hold PATH_MAX) or, if RESOLVED is NULL, a malloc'd buffer; returns
 * it, or NULL with errno set (ENOENT when the path does not exist) — POSIX-faithful for ports. */
char* realpath(const char* path, char* resolved) {
	if (!path || !path[0]) { errno = ENOENT; return 0; }

	char abs[PATH_MAX];
	int n = 0;
	if (path[0] != '/') {                       /* relative -> prepend cwd */
		if (!getcwd(abs, sizeof abs)) return 0;
		n = (int) strlen(abs);
	}
	for (const char* p = path; *p && n < PATH_MAX - 1; p++) {
		if (n == 0 || abs[n - 1] != '/' || *p != '/')   /* fold runs of '/' as we copy */
			abs[n++] = *p;
	}
	abs[n] = 0;

	char out[PATH_MAX];                         /* canonical result, built component by component */
	int o = 0;
	const char* s = abs;
	while (*s) {
		while (*s == '/') s++;
		if (!*s) break;
		const char* start = s;
		while (*s && *s != '/') s++;
		int len = (int) (s - start);
		if (len == 1 && start[0] == '.') {
			continue;                           /* "." -> no-op */
		} else if (len == 2 && start[0] == '.' && start[1] == '.') {
			while (o > 0 && out[o - 1] != '/') o--;   /* drop last component name */
			if (o > 0) o--;                            /* and its leading slash */
		} else {
			if (o < PATH_MAX - 1) out[o++] = '/';
			for (int i = 0; i < len && o < PATH_MAX - 1; i++) out[o++] = start[i];
		}
	}
	if (o == 0) out[o++] = '/';                 /* everything collapsed -> root */
	out[o] = 0;

	struct stat st;
	if (stat(out, &st) != 0) return 0;          /* errno (ENOENT/…) set by stat */

	char* dst = resolved ? resolved : (char*) malloc((size_t) o + 1);
	if (!dst) { errno = ENOMEM; return 0; }
	strcpy(dst, out);
	return dst;
}

/* dlfcn: NanOS has no runtime shared-object loading; dlopen() always fails so callers (htop's
 * SystemdMeter, which dlopens libsystemd.so.0) degrade to "feature unavailable". */
void* dlopen(const char* file, int mode) { (void) file; (void) mode; return 0; }
void* dlsym(void* handle, const char* name) { (void) handle; (void) name; return 0; }
int   dlclose(void* handle) { (void) handle; return 0; }
char* dlerror(void) { return (char*) "dynamic loading not supported on NanOS"; }

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

/* alarm(2): arm a one-shot ITIMER_REAL for `sec` seconds (0 cancels), returning the seconds
 * left on any previously-set alarm — the standard implementation over setitimer/getitimer
 * (now that the kernel has a real interval timer that fires SIGALRM). */
unsigned alarm(unsigned sec) {
	struct itimerval nv, ov;
	nv.it_interval.tv_sec = 0; nv.it_interval.tv_usec = 0;   /* one-shot */
	nv.it_value.tv_sec = (time_t) sec; nv.it_value.tv_usec = 0;
	if (setitimer(ITIMER_REAL, &nv, &ov) < 0) return 0;
	unsigned left = (unsigned) ov.it_value.tv_sec;
	if (left == 0 && ov.it_value.tv_usec != 0) left = 1;     /* round a sub-second remainder up */
	return left;
}
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

/* user/group database: getpwuid/getpwnam live in pwd_grp.c; getgrgid/getgrnam/getgrent/
 * setgrent/endgrent + getspnam + getgrouplist/initgroups live in grp_shadow.c (real /etc/group
 * and /etc/shadow parsing). Only the passwd-enumeration remainders stay here (rarely used). */
struct passwd* getpwent(void) { return 0; }
void setpwent(void) {}
void endpwent(void) {}
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

/* Extended attributes: NanOS has no xattr support. Stub the Linux 4-arg family toybox calls
 * (lib/portability.c) as ENOTSUP — programs degrade to "no attributes". */
#include <sys/types.h>
ssize_t getxattr(const char* p, const char* n, void* v, size_t s) { (void)p;(void)n;(void)v;(void)s; errno = ENOTSUP; return -1; }
ssize_t lgetxattr(const char* p, const char* n, void* v, size_t s) { (void)p;(void)n;(void)v;(void)s; errno = ENOTSUP; return -1; }
ssize_t fgetxattr(int fd, const char* n, void* v, size_t s) { (void)fd;(void)n;(void)v;(void)s; errno = ENOTSUP; return -1; }
ssize_t listxattr(const char* p, char* l, size_t s) { (void)p;(void)l;(void)s; return 0; }
ssize_t llistxattr(const char* p, char* l, size_t s) { (void)p;(void)l;(void)s; return 0; }
ssize_t flistxattr(int fd, char* l, size_t s) { (void)fd;(void)l;(void)s; return 0; }
int setxattr(const char* p, const char* n, const void* v, size_t s, int f) { (void)p;(void)n;(void)v;(void)s;(void)f; errno = ENOTSUP; return -1; }
int lsetxattr(const char* p, const char* n, const void* v, size_t s, int f) { (void)p;(void)n;(void)v;(void)s;(void)f; errno = ENOTSUP; return -1; }
int fsetxattr(int fd, const char* n, const void* v, size_t s, int f) { (void)fd;(void)n;(void)v;(void)s;(void)f; errno = ENOTSUP; return -1; }

/* mprotect: NanOS userland cannot change page protections (the kernel maps user pages fixed).
 * sudo calls it only as a hardening measure (making its policy memory read-only); report success
 * so sudo proceeds — the memory simply stays as mapped. */
#include <sys/mman.h>
int mprotect(void* addr, size_t len, int prot) { (void) addr; (void) len; (void) prot; return 0; }

/* open_memstream: a GNU growable-buffer output stream. picolibc's tinystdio has no custom-stream
 * primitive (no fopencookie/funopen), so a faithful growable FILE* cannot be built here. Report
 * "unsupported" honestly (NULL + ENOSYS). The only consumers in our stack are libdrm's cosmetic
 * drmGetFormatModifierName* helpers, which already return NULL when the stream can't be created;
 * they are never on the virgl/Mesa render path. */
#include <stdio.h>
FILE *open_memstream(char **ptr, size_t *sizeloc) { (void) ptr; (void) sizeloc; errno = ENOSYS; return NULL; }

/* memfd_create: an anonymous memory-backed fd. NanOS /tmp is RamFs (memory), so a uniquely-named
 * file created there and immediately unlinked is exactly that — the fd keeps the memory file alive
 * with no directory entry. Mesa/GBM use it as an mmap-able, ftruncate-able buffer. Seal flags
 * (MFD_ALLOW_SEALING) are accepted but not enforced (no F_ADD_SEALS); MFD_CLOEXEC is a no-op. */
#include <fcntl.h>
int memfd_create(const char *name, unsigned int flags) {
	static unsigned ctr;
	char path[64];
	int fd;
	(void) name; (void) flags;
	snprintf(path, sizeof path, "/tmp/.memfd-%d-%u", (int) getpid(), ctr++);
	fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0) return -1;
	unlink(path);
	return fd;
}

/* dl_iterate_phdr: NanOS has no glibc-style shared-object phdr chain (single .nxe per process +
 * the DynLoader for .ndl modules), so iterate nothing and return 0. The only consumer, Mesa's
 * build_id.c, then finds no ELF build-id — harmless (the shader cache that would use it is off). */
#include <link.h>
int dl_iterate_phdr(int (*cb)(struct dl_phdr_info *, size_t, void *), void *data) {
	(void) cb; (void) data;
	return 0;
}

/* dladdr: no runtime symbol/object table on NanOS -> report "not found" (0). Mesa's build_id.c
 * falls back gracefully (no build-id; the shader cache that would use it is disabled). */
#include <dlfcn.h>
int dladdr(const void *addr, Dl_info *info) { (void) addr; if (info) { info->dli_fname=0; info->dli_fbase=0; info->dli_sname=0; info->dli_saddr=0; } return 0; }


/* mincore: NanOS eagerly backs every mapping (no reclaim), so all queried pages are resident. */
int mincore(void *addr, size_t length, unsigned char *vec) {
	size_t pages = (length + 4095) / 4096, i;
	(void) addr;
	if (vec) for (i = 0; i < pages; i++) vec[i] = 1;
	return 0;
}

/* CPU affinity: report a single schedulable CPU (Mesa sizes thread pools from CPU_COUNT; our
 * libstdc++ has threads disabled, so single-threaded is correct). sched_yield: no-op. */
#include <sched.h>
int sched_getaffinity(int pid, size_t sz, cpu_set_t *m) { (void) pid; (void) sz; if (m) { CPU_ZERO(m); CPU_SET(0, m); } return 0; }
int sched_yield(void) { return 0; }

/* pthread_setname_np: thread debug name — accepted, ignored. */
#include <pthread.h>
int pthread_setname_np(pthread_t t, const char *n) { (void) t; (void) n; return 0; }

/* popen/pclose: no shell/process pipes on NanOS -> report unsupported (callers degrade). */
#include <stdio.h>
FILE *popen(const char *cmd, const char *mode) { (void) cmd; (void) mode; errno = ENOSYS; return NULL; }
int   pclose(FILE *f) { (void) f; return -1; }

/* sched_getcpu: single-CPU view for the port (see sched_getaffinity). */
int sched_getcpu(void) { return 0; }

/* --- Mesa (gallium-virgl + EGL) bring-up gaps --------------------------------------------------
 * These five are referenced by unmodified Mesa 24.2 and were the only genuinely-missing libc
 * symbols in the gles2info link (the stdout/stderr/_ctype_b data imports are handled by the
 * dllimport shim, not here). Real implementations where NanOS can back them; honest degradations
 * where it cannot. */
#include <time.h>

/* secure_getenv: NanOS draws no setuid-tainted-environment distinction here, so it is exactly
 * getenv. Mesa reads MESA_ and GALLIUM_ driver tunables through it. */
char *secure_getenv(const char *name) { return getenv(name); }

/* pthread_condattr: the NanOS pthread cond (vendored musl) encodes the clock in __attr —
 * pthread_cond_init does `_c_clock = __attr & 0x7fffffff; shared = __attr>>31`. Mirror that so a
 * CLOCK_MONOTONIC cond built by Mesa's dri2 sync path actually waits on the monotonic clock. */
int pthread_condattr_init(pthread_condattr_t *a)    { if (a) a->__attr = 0; return 0; }
int pthread_condattr_destroy(pthread_condattr_t *a) { (void) a; return 0; }
int pthread_condattr_setclock(pthread_condattr_t *a, clockid_t clk) {
	if (!a) return EINVAL;
	a->__attr = (a->__attr & 0x80000000u) | ((unsigned) clk & 0x7fffffffu);
	return 0;
}

/* pthread_getcpuclockid: NanOS has no true per-thread CPU clock; report CLOCK_THREAD_CPUTIME_ID
 * and let clock_gettime decide. Mesa uses this only for optional profiling counters. */
#ifndef CLOCK_THREAD_CPUTIME_ID
#define CLOCK_THREAD_CPUTIME_ID 3
#endif
int pthread_getcpuclockid(pthread_t t, clockid_t *clk) { (void) t; if (clk) *clk = CLOCK_THREAD_CPUTIME_ID; return 0; }

/* clock_nanosleep: the kernel exposes only relative nanosleep, so the absolute (TIMER_ABSTIME)
 * form subtracts the current time of the requested clock. Returns 0 or a positive errno (POSIX).
 * Mesa's os_time throttling uses the CLOCK_MONOTONIC absolute form. */
#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 1
#endif
int clock_nanosleep(clockid_t clk, int flags, const struct timespec *req, struct timespec *rem) {
	struct timespec rel;
	if (!req) return EINVAL;
	if (flags & TIMER_ABSTIME) {
		struct timespec now;
		if (clock_gettime(clk, &now) != 0) return EINVAL;
		rel.tv_sec  = req->tv_sec  - now.tv_sec;
		rel.tv_nsec = req->tv_nsec - now.tv_nsec;
		if (rel.tv_nsec < 0) { rel.tv_sec--; rel.tv_nsec += 1000000000L; }
		if (rel.tv_sec < 0 || (rel.tv_sec == 0 && rel.tv_nsec <= 0)) return 0; /* deadline passed */
		rem = NULL; /* remaining is undefined for the absolute form */
	} else {
		rel = *req;
	}
	return nanosleep(&rel, rem) == 0 ? 0 : errno;
}

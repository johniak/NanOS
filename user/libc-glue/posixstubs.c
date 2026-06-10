/*
 * posixstubs.c — POSIX functions bash (and other ports) reference that picolibc does not
 * provide. NanOS is single-user (everything is root) with a read-only disk + /tmp, so the
 * identity/permission calls are honest no-ops and the user/group database resolves only
 * root. These live in libc.ndl alongside the syscall glue.
 */
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <pwd.h>
#include <grp.h>

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

/* ---- permission ops on a read-only world: accept, do nothing ---- */
int chown(const char* p, uid_t u, gid_t g)  { (void) p; (void) u; (void) g; return 0; }
int fchown(int fd, uid_t u, gid_t g)        { (void) fd; (void) u; (void) g; return 0; }
int chmod(const char* p, mode_t m)          { (void) p; (void) m; return 0; }
int fchmod(int fd, mode_t m)                { (void) fd; (void) m; return 0; }

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

/* user/group database: only root (uid/gid 0) exists; enumeration is empty. The name lookups
 * delegate to getpwuid/getgrgid (pwd_grp.c) so the fully-populated root entry has a single
 * source of truth (every string field non-NULL — a shell strlen()s pw_dir/pw_shell). */
struct group* getgrgid(gid_t);
struct passwd* getpwnam(const char* name) {
	if (!name || strcmp(name, "root") != 0) return 0;
	return getpwuid(0);
}
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

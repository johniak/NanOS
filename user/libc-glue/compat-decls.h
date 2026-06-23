/*
 * compat-decls.h — declarations picolibc omits for the i686-elf target but that
 * the verbatim sbase sources reference. Force-included via -include so the vendored
 * programs stay unmodified. The definitions live in user/libc-glue/syscalls.c.
 */
#ifndef NX_COMPAT_DECLS_H
#define NX_COMPAT_DECLS_H

/* string.h up front: some ports (sudo) have a TU that uses memcpy/strlen without including it
 * directly, relying on transitive Linux-header pulls that picolibc doesn't replicate. */
#include <string.h>

struct stat;
int lstat(const char* path, struct stat* buf);
/* renameat: implemented in syscalls.c (SYS_renameat) but picolibc's <stdio.h> doesn't declare it
 * where sudo uses it. */
int renameat(int oldfd, const char* oldpath, int newfd, const char* newpath);

/* mknod: picolibc declares mknodat but not mknod for i686-elf; cp.c (compiled as C++) needs a
 * declaration or the call is a hard error. The (ENOSYS) definition lives in posixstubs.c. */
#include <sys/types.h>
int mknod(const char* path, mode_t mode, dev_t dev);

/* picolibc gates the POSIX timer API behind a feature macro that our freestanding
 * build doesn't set, so it declares neither clock_gettime/nanosleep nor the CLOCK_*
 * ids — but it DOES define struct timespec/clockid_t. Expose them here (impl in
 * user/libc-glue/syscalls.c). NanOS has a single monotonic clock, so the id is moot. */
#include <time.h>
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
int clock_gettime(clockid_t clk, struct timespec* tp);
int nanosleep(const struct timespec* req, struct timespec* rem);

/* utimensat/futimens sentinels: picolibc defines UTIME_NOW/OMIT only for Cygwin/RTEMS, so they
 * are absent for i686-elf. Use the real Linux tv_nsec encoding — exactly what the NanOS kernel
 * recognizes (kernel/Syscall.cpp utimensat). touch.c needs these in a static initializer. */
#ifndef UTIME_NOW
#define UTIME_NOW  0x3fffffff
#endif
#ifndef UTIME_OMIT
#define UTIME_OMIT 0x3ffffffe
#endif
/* strptime: picolibc declares it only under __XSI_VISIBLE, which the freestanding build doesn't
 * set; the symbol IS in libc.a. struct tm is complete here (via <time.h> above), so declare it. */
char* strptime(const char* s, const char* fmt, struct tm* tm);

/* picolibc's <sys/resource.h> ships only getrusage/struct rusage, not the rlimit surface.
 * Mirror the minimal struct the getrlimit/setrlimit glue (posixstubs.c) implements; the SDK
 * sysroot adds the same declarations (+ RLIMIT_*) for external ports. */
typedef unsigned long rlim_t;
struct rlimit { rlim_t rlim_cur; rlim_t rlim_max; };
#ifndef RLIM_INFINITY
#define RLIM_INFINITY (~0UL)
#endif
/* RLIMIT_* ids + getrlimit/setrlimit decls (picolibc omits them; sudo references RLIMIT_NOFILE
 * etc.). Implemented in posixstubs.c (everything is reported unlimited). */
#ifndef RLIMIT_CPU
#define RLIMIT_CPU    0
#define RLIMIT_FSIZE  1
#define RLIMIT_DATA   2
#define RLIMIT_STACK  3
#define RLIMIT_CORE   4
#define RLIMIT_RSS    5
#define RLIMIT_NPROC  6
#define RLIMIT_NOFILE 7
#define RLIMIT_MEMLOCK 8
#define RLIMIT_AS     9
#define RLIM_NLIMITS  16
#endif
int getrlimit(int resource, struct rlimit* rl);
int setrlimit(int resource, const struct rlimit* rl);
/* getpriority/setpriority + PRIO_* (picolibc omits them; sudo lowers its own priority).
 * Implemented in posixstubs.c (no-op: NanOS has no nice levels). */
#ifndef PRIO_PROCESS
#define PRIO_PROCESS 0
#define PRIO_PGRP    1
#define PRIO_USER    2
#endif
int getpriority(int which, int who);
int setpriority(int which, int who, int prio);

/* getprogname/setprogname (BSD): picolibc doesn't declare them; the SDK sysroot adds the same
 * decls for external ports. The crt0 hook __nx_set_progname seeds it from argv[0]. */
const char* getprogname(void);
void setprogname(const char* p);
void __nx_set_progname(const char* argv0);

/* --- toybox port compat (picolibc gaps) --------------------------------------------------- */
/* sigjmp_buf/sigsetjmp/siglongjmp: picolibc has setjmp but not the signal-mask variants. NanOS
 * has no saved signal mask across longjmp, so map them onto plain setjmp/longjmp (the `savemask`
 * argument is ignored). toybox's toy_context embeds a sigjmp_buf. */
#include <setjmp.h>
#ifndef sigjmp_buf
typedef jmp_buf sigjmp_buf;
#define sigsetjmp(env, savemask) setjmp(env)
#define siglongjmp(env, val)     longjmp(env, val)
#endif

/* dprintf/vdprintf: picolibc's <stdio.h> doesn't declare them for the freestanding build (the
 * symbols are in libc.a). toybox's xprintf layer uses dprintf. */
#include <stdarg.h>
int dprintf(int fd, const char* fmt, ...);
int vdprintf(int fd, const char* fmt, va_list ap);

/* _PATH_DEFPATH: picolibc's <paths.h> omits it; login/su seed the session PATH from it. */
#include <paths.h>
#ifndef _PATH_DEFPATH
#define _PATH_DEFPATH "/disks/main/nanos/bin:/disks/main/bin"
#endif
/* a handful of _PATH_* picolibc's <paths.h> omits (sudo/visudo reference them). */
#ifndef _PATH_VI
#define _PATH_VI "/disks/main/nanos/bin/vi.nxe"
#endif
#ifndef _PATH_DEV
#define _PATH_DEV "/dev/"
#endif
#ifndef _PATH_TTY
#define _PATH_TTY "/dev/tty"
#endif
#ifndef _PATH_DEVNULL
#define _PATH_DEVNULL "/dev/null"
#endif

/* WIFCONTINUED: picolibc's <sys/wait.h> omits it (sudo's exec wait loop uses it). NanOS uses the
 * glibc wait-status encoding (0xffff == "continued"), matching nsh's W* decoders. */
#include <sys/wait.h>
#ifndef WIFCONTINUED
#define WIFCONTINUED(s) ((s) == 0xffff)
#endif

/* cfsetspeed: picolibc termios declares cfsetispeed/cfsetospeed but not the combined setter;
 * toybox lib/tty.c uses it. Implemented in libc-glue/termios.c. */
#include <termios.h>
int cfsetspeed(struct termios* t, speed_t s);

/* getgrouplist: picolibc's <grp.h> omits it; toybox id/groups use it. Defined in grp_shadow.c. */
#include <sys/types.h>
int getgrouplist(const char* user, gid_t group, gid_t* groups, int* ngroups);

/* xattr family: NanOS has no extended attributes; toybox lib/portability.c calls the Linux
 * 4-arg getxattr/setxattr/listxattr. Declared here, stubbed (-1/ENOTSUP) in posixstubs.c. */
ssize_t getxattr(const char* path, const char* name, void* value, size_t size);
ssize_t lgetxattr(const char* path, const char* name, void* value, size_t size);
ssize_t fgetxattr(int fd, const char* name, void* value, size_t size);
ssize_t listxattr(const char* path, char* list, size_t size);
ssize_t llistxattr(const char* path, char* list, size_t size);
ssize_t flistxattr(int fd, char* list, size_t size);
int setxattr(const char* path, const char* name, const void* value, size_t size, int flags);
int lsetxattr(const char* path, const char* name, const void* value, size_t size, int flags);
int fsetxattr(int fd, const char* name, const void* value, size_t size, int flags);

#endif /* NX_COMPAT_DECLS_H */

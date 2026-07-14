/*
 * sys/resource.h — NanOS replacement for picolibc's minimal version. picolibc's struct rusage has
 * only ru_utime/ru_stime; libuv's uv_getrusage fills the full glibc/BSD set, so this provides the
 * complete struct (getrusage zeroes the extended fields — NanOS has no per-process accounting yet).
 * The rlimit/priority surface matches the NanOS picolibc patch this replaces.
 */
#ifndef _SYS_RESOURCE_H_
#define _SYS_RESOURCE_H_

#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN -1
#define RUSAGE_THREAD   1

struct rusage {
	struct timeval ru_utime;   /* user CPU time used */
	struct timeval ru_stime;   /* system CPU time used */
	long ru_maxrss;            /* maximum resident set size */
	long ru_ixrss;             /* integral shared memory size */
	long ru_idrss;             /* integral unshared data size */
	long ru_isrss;             /* integral unshared stack size */
	long ru_minflt;            /* page reclaims (soft page faults) */
	long ru_majflt;            /* page faults (hard page faults) */
	long ru_nswap;             /* swaps */
	long ru_inblock;           /* block input operations */
	long ru_oublock;           /* block output operations */
	long ru_msgsnd;            /* IPC messages sent */
	long ru_msgrcv;            /* IPC messages received */
	long ru_nsignals;          /* signals received */
	long ru_nvcsw;             /* voluntary context switches */
	long ru_nivcsw;            /* involuntary context switches */
};

int getrusage(int who, struct rusage* usage);

typedef unsigned long rlim_t;
struct rlimit { rlim_t rlim_cur; rlim_t rlim_max; };
#define RLIM_INFINITY (~0UL)
#define RLIMIT_CPU     0
#define RLIMIT_FSIZE   1
#define RLIMIT_DATA    2
#define RLIMIT_STACK   3
#define RLIMIT_CORE    4
#define RLIMIT_NOFILE  5
#define RLIMIT_AS      6
#define RLIMIT_NPROC   7
#define RLIMIT_MEMLOCK 8
#define RLIMIT_RSS     9    /* resident set size (node_report iterates rlimits) */
#define RLIMIT_LOCKS   10
#define RLIMIT_SIGPENDING 11
#define RLIMIT_MSGQUEUE 12
#define RLIMIT_NICE    13
#define RLIMIT_RTPRIO  14
#define RLIM_NLIMITS   15
/* No rlim_t typedef: autoconf apps #define their own when missing, and the struct uses unsigned long
 * directly, so we avoid clashing with that. */
int getrlimit(int, struct rlimit*);
int setrlimit(int, const struct rlimit*);
#define PRIO_PROCESS 0
#define PRIO_PGRP    1
#define PRIO_USER    2
int getpriority(int, int);
int setpriority(int, int, int);

#ifdef __cplusplus
}
#endif

#endif /* !_SYS_RESOURCE_H_ */

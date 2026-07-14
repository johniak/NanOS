/* sched.h — CPU affinity set for ports that size thread pools via sched_getaffinity/CPU_COUNT
 * (Mesa's u_cpu_detect) + the POSIX scheduling parameter/policy surface (abseil/V8/pthread read a
 * thread's priority). NanOS has one scheduling policy and no thread priorities, so the scheduling
 * bits are constant. */
#ifndef _NANOS_SCHED_H
#define _NANOS_SCHED_H
#include <stddef.h>

/* Scheduling policies + parameter block. NanOS has a single policy and does not honour priorities;
 * struct sched_param exists so <pthread.h>'s *schedparam declarations complete, and the getters
 * report a constant (priority 0, SCHED_OTHER). */
#define SCHED_OTHER 0
#define SCHED_FIFO  1
#define SCHED_RR    2
struct sched_param { int sched_priority; };
int sched_get_priority_max(int policy);
int sched_get_priority_min(int policy);
int sched_getparam(int pid, struct sched_param *param);
int sched_setscheduler(int pid, int policy, const struct sched_param *param);
#define CPU_SETSIZE 1024
#define __NCPUBITS (8 * sizeof(unsigned long))
typedef struct { unsigned long __bits[CPU_SETSIZE / __NCPUBITS]; } cpu_set_t;
#define CPU_ZERO(s)     __builtin_memset((s), 0, sizeof(cpu_set_t))
#define CPU_SET(c, s)   ((s)->__bits[(c) / __NCPUBITS] |= 1UL << ((c) % __NCPUBITS))
#define CPU_CLR(c, s)   ((s)->__bits[(c) / __NCPUBITS] &= ~(1UL << ((c) % __NCPUBITS)))
#define CPU_ISSET(c, s) ((((s)->__bits[(c) / __NCPUBITS]) >> ((c) % __NCPUBITS)) & 1UL)
static inline int __cpu_count(const cpu_set_t *s) {
	int n = 0; size_t i;
	for (i = 0; i < sizeof(cpu_set_t) / sizeof(unsigned long); i++) {
		unsigned long b = s->__bits[i]; while (b) { n += (int)(b & 1UL); b >>= 1; }
	}
	return n;
}
#define CPU_COUNT(s) __cpu_count(s)
int sched_getaffinity(int pid, size_t cpusetsize, cpu_set_t *mask);
int sched_yield(void);
int sched_getcpu(void);
#endif

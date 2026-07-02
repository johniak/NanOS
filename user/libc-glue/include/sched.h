/* sched.h — CPU affinity set for ports that size thread pools via sched_getaffinity/CPU_COUNT
 * (Mesa's u_cpu_detect). NanOS reports a single schedulable CPU here (libstdc++ threads are off),
 * so pools stay single-threaded. */
#ifndef _NANOS_SCHED_H
#define _NANOS_SCHED_H
#include <stddef.h>
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

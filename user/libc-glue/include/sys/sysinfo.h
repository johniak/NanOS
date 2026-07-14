/*
 * sys/sysinfo.h — sysinfo(2) system statistics. libuv reads totalram/freeram (uv_get_total_memory /
 * uv_get_free_memory) and uptime. NanOS's libc-glue fills the memory fields from /proc/meminfo and
 * uptime from /proc/uptime; the rest are zero (no load average / swap accounting yet).
 */
#ifndef _SYS_SYSINFO_H
#define _SYS_SYSINFO_H

#ifdef __cplusplus
extern "C" {
#endif

struct sysinfo {
	long uptime;                 /* seconds since boot */
	unsigned long loads[3];      /* 1/5/15-minute load, << SI_LOAD_SHIFT */
	unsigned long totalram;      /* total usable main memory */
	unsigned long freeram;       /* available memory */
	unsigned long sharedram;
	unsigned long bufferram;
	unsigned long totalswap;
	unsigned long freeswap;
	unsigned short procs;        /* number of processes */
	unsigned short pad;
	unsigned long totalhigh;
	unsigned long freehigh;
	unsigned int mem_unit;       /* size of a memory unit in bytes */
	char _f[20 - 2 * sizeof(long) - sizeof(int)];
};

int get_nprocs(void);
int get_nprocs_conf(void);
int sysinfo(struct sysinfo* info);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_SYSINFO_H */

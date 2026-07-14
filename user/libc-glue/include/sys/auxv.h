/*
 * sys/auxv.h — getauxval(3). Node reads AT_SECURE (was the process started set-uid?) and V8 reads
 * AT_HWCAP. NanOS has no ELF auxiliary vector exposed to userland, so getauxval returns 0 for every
 * key — AT_SECURE 0 means "not suid" (every NanOS process runs as root anyway), and AT_HWCAP 0 makes
 * V8 fall back to CPUID on x86_64.
 */
#ifndef _SYS_AUXV_H
#define _SYS_AUXV_H

#include <linux/auxvec.h>

#ifdef __cplusplus
extern "C" {
#endif

unsigned long getauxval(unsigned long type);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_AUXV_H */

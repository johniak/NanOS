/*
 * linuxkpi/lkpi_knx.h — declarations of the NanOS kernel facilities the LinuxKPI shim
 * imports (the knx_* ABI from kernel/kexports.def). Wrapped in a conditional extern "C"
 * so the SAME shim sources work both as C (the real .nkext build) and as C++ (the host
 * doctest harness compiles .c with g++). In the kext, these resolve to the kernel export
 * table; on the host, to test shims forwarding to libc.
 */
#ifndef _LINUXKPI_LKPI_KNX_H
#define _LINUXKPI_LKPI_KNX_H

#ifdef __cplusplus
extern "C" {
#endif

void              *knx_malloc(unsigned size);
void               knx_free(void *p);
void               knx_log(const char *s);
unsigned long long knx_uptime_us(void);

#ifdef __cplusplus
}
#endif

#endif /* _LINUXKPI_LKPI_KNX_H */

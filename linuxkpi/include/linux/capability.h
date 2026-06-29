#ifndef _LKPI_CAPABILITY_H
#define _LKPI_CAPABILITY_H
#define CAP_SYS_ADMIN 21
#define CAP_SYS_RAWIO 17
#define CAP_DAC_OVERRIDE 1
static inline int perfmon_capable(void){ return 1; }
#endif

/* linuxkpi/include/linux/suspend.h — system suspend hooks. NanOS does not suspend during bring-up. */
#ifndef _LINUXKPI_LINUX_SUSPEND_H
#define _LINUXKPI_LINUX_SUSPEND_H
static inline int pm_suspend_target_state(void){ return 0; }
#define PM_SUSPEND_ON 0
#endif

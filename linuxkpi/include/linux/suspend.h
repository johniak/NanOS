/* linuxkpi/include/linux/suspend.h — system suspend hooks. NanOS does not suspend during bring-up. */
#ifndef _LINUXKPI_LINUX_SUSPEND_H
#define _LINUXKPI_LINUX_SUSPEND_H
static inline int pm_suspend_target_state(void){ return 0; }
typedef int suspend_state_t;
#define PM_SUSPEND_ON      0
#define PM_SUSPEND_TO_IDLE 1
#define PM_SUSPEND_STANDBY 2
#define PM_SUSPEND_MEM     3
#define PM_SUSPEND_MAX     4
static inline bool pm_suspend_via_firmware(void){ return false; }
static inline bool pm_resume_via_firmware(void){ return false; }
#endif

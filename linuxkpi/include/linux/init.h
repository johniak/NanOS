/*
 * linuxkpi/include/linux/init.h — init section annotations + initcall stubs for the shim.
 */
#ifndef _LINUXKPI_LINUX_INIT_H
#define _LINUXKPI_LINUX_INIT_H

#include <linux/compiler.h>

#ifndef __init
#define __init
#endif
#ifndef __exit
#define __exit
#endif
#define __initdata
#define __exitdata
#define __initconst
#define __devinit
#define __refdata

typedef int (*initcall_t)(void);

/* initcalls are not auto-run; the module entry calls its init explicitly. */
#define core_initcall(fn)
#define postcore_initcall(fn)
#define arch_initcall(fn)
#define subsys_initcall(fn)
#define fs_initcall(fn)
#define device_initcall(fn)
#define late_initcall(fn)
#define __initcall(fn)
#define pure_initcall(fn)

#endif /* _LINUXKPI_LINUX_INIT_H */

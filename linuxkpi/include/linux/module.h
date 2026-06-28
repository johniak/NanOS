/*
 * linuxkpi/include/linux/module.h — module macros for the shim (all no-ops; the driver is
 * statically linked into virtio_gpu.nkext and entered via nkext_init, not module_init).
 */
#ifndef _LINUXKPI_LINUX_MODULE_H
#define _LINUXKPI_LINUX_MODULE_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/export.h>
#include <linux/init.h>

struct module;

#define MODULE_AUTHOR(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_LICENSE(x)
#define MODULE_VERSION(x)
#define MODULE_ALIAS(x)
#define MODULE_DEVICE_TABLE(type, name)
#define MODULE_FIRMWARE(x)
#define MODULE_PARM_DESC(a, b)
#define module_param(a, b, c)
#define module_param_named(a, b, c, d)

#define module_init(fn) int __maybe_unused __lkpi_modinit_##fn(void) { return fn(); }
#define module_exit(fn) void __maybe_unused __lkpi_modexit_##fn(void) { fn(); }

#define try_module_get(m) (true)
#define module_put(m)     do {} while (0)
#define __module_get(m)   do {} while (0)

#endif /* _LINUXKPI_LINUX_MODULE_H */

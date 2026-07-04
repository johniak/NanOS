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
#define module_param_named_unsafe(a, b, c, d)
#define module_param_unsafe(a, b, c)
#define module_param_string(a, b, c, d)
#define module_param_array(a, b, c, d)
#define module_param_cb(a, b, c, d)
#define core_param(a, b, c, d)

#define module_init(fn) int __maybe_unused __lkpi_modinit_##fn(void) { return fn(); }
#define module_exit(fn) void __maybe_unused __lkpi_modexit_##fn(void) { fn(); }

#define try_module_get(m) (true)
#define module_put(m)     do {} while (0)
#define __module_get(m)   do {} while (0)

/* Optional inter-module symbol coupling. Real Linux resolves these against the loaded-
 * module symbol table at runtime; we have no such table, so an optional symbol is always
 * ABSENT (NULL). That is the correct semantic here: i915's intel_rps.c uses
 * symbol_get(ips_link_to_i915_driver) to reach the Gen5-only intel_ips.ko, which never
 * exists on our target (Gen9.5) — so the coupling is a no-op, exactly as when IPS is
 * unloaded upstream. typeof(&x) keeps the NULL correctly typed for the caller's pointer. */
#define symbol_get(x)  ((typeof(&(x)))0)
#define symbol_put(x)  do {} while (0)

#endif /* _LINUXKPI_LINUX_MODULE_H */

#ifndef _LKPI_MODULE_DRIVER
#define _LKPI_MODULE_DRIVER
/* module_driver: emit a GLOBAL, well-known init/exit entry the kext bootstrap calls
 * explicitly (lkpi_module_init/exit). This is what lets us run the driver UNMODIFIED:
 * its `static struct virtio_driver foo` and the module_*_driver() macro stay as-is, and
 * the registration is reachable from the kext entry through these stable wrapper names.
 * (One module_driver() per .nkext, which holds for our single-driver modules.) */
#define module_driver(__driver, __register, __unregister, ...) \
  int lkpi_module_init(void) { return __register(&(__driver), ##__VA_ARGS__); } \
  void lkpi_module_exit(void) { __unregister(&(__driver)); }
#define module_pci_driver(__pci_driver) \
  module_driver(__pci_driver, pci_register_driver, pci_unregister_driver)
#endif

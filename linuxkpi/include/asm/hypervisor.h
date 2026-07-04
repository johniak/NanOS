/* linuxkpi/include/asm/hypervisor.h — hypervisor detection. On the target (Dell, bare metal) the
 * platform is NATIVE, so hypervisor_is_type(X86_HYPER_NATIVE) is true and i915_run_as_guest() is
 * false — the correct answer for real hardware (i915 does not run under QEMU here). */
#ifndef _LKPI_ASM_HYPERVISOR_H
#define _LKPI_ASM_HYPERVISOR_H
#include <linux/types.h>
enum x86_hypervisor_type { X86_HYPER_NATIVE = 0, X86_HYPER_VMWARE, X86_HYPER_MS_HYPERV,
	X86_HYPER_XEN_PV, X86_HYPER_XEN_HVM, X86_HYPER_KVM, X86_HYPER_JAILHOUSE, X86_HYPER_ACRN };
static inline bool hypervisor_is_type(enum x86_hypervisor_type type) { return type == X86_HYPER_NATIVE; }
#endif

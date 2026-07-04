/* linuxkpi/include/linux/fault-inject.h — CONFIG_FAULT_INJECTION=off: should_fail* always false. */
#ifndef _LINUXKPI_LINUX_FAULT_INJECT_H
#define _LINUXKPI_LINUX_FAULT_INJECT_H
#include <linux/types.h>
struct fault_attr { int dummy; };
#define DECLARE_FAULT_ATTR(name) struct fault_attr name = { 0 }
static inline bool should_fail(struct fault_attr *a, ssize_t s){ (void)a;(void)s; return false; }
static inline bool should_fail_ex(struct fault_attr *a, ssize_t s, int f){ (void)a;(void)s;(void)f; return false; }
#endif

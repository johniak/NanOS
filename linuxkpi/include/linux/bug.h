/*
 * linuxkpi/include/linux/bug.h — BUG/WARN for the shim. BUG halts; WARN logs and continues.
 */
#ifndef _LINUXKPI_LINUX_BUG_H
#define _LINUXKPI_LINUX_BUG_H

#include <linux/compiler.h>
#include <linux/printk.h>
#include <linux/build_bug.h>

#define BUG() do { printk("BUG at %s:%d\n", __FILE__, __LINE__); for (;;) __asm__ __volatile__("hlt"); } while (0)
#define BUG_ON(cond) do { if (unlikely(cond)) BUG(); } while (0)

#define WARN_ON(cond) ({ int __c = !!(cond); if (__c) printk("WARN at %s:%d\n", __FILE__, __LINE__); __c; })
#define WARN_ON_ONCE(cond) ({ static int __w; int __c = !!(cond); if (__c && !__w) { __w = 1; printk("WARN_ONCE at %s:%d\n", __FILE__, __LINE__); } __c; })
#define WARN(cond, fmt, ...) ({ int __c = !!(cond); if (__c) printk(fmt, ##__VA_ARGS__); __c; })
#define WARN_ONCE(cond, fmt, ...) ({ static int __w; int __c = !!(cond); if (__c && !__w) { __w = 1; printk(fmt, ##__VA_ARGS__); } __c; })
#define WARN_ON_SMP(cond) WARN_ON(cond)

#endif /* _LINUXKPI_LINUX_BUG_H */

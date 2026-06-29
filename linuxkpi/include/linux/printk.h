/*
 * linuxkpi/include/linux/printk.h — kernel logging + formatting for the LinuxKPI shim.
 * Formatting is a self-contained vsnprintf subset (kpi_print.c); output goes to knx_log.
 */
#ifndef _LINUXKPI_LINUX_PRINTK_H
#define _LINUXKPI_LINUX_PRINTK_H

#include <linux/types.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

int vscnprintf(char *buf, size_t size, const char *fmt, va_list args);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list args);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int scnprintf(char *buf, size_t size, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int printk(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

/* Linux log-level prefixes are ignored by our printk (it strips a leading "\001N"). */
#define KERN_SOH      "\001"
#define KERN_EMERG    KERN_SOH "0"
#define KERN_ALERT    KERN_SOH "1"
#define KERN_CRIT     KERN_SOH "2"
#define KERN_ERR      KERN_SOH "3"
#define KERN_WARNING  KERN_SOH "4"
#define KERN_NOTICE   KERN_SOH "5"
#define KERN_INFO     KERN_SOH "6"
#define KERN_DEBUG    KERN_SOH "7"
#define KERN_DEFAULT  ""
#define KERN_CONT     ""

#define pr_fmt(fmt) fmt

#define pr_emerg(fmt, ...)   printk(KERN_EMERG   pr_fmt(fmt), ##__VA_ARGS__)
#define pr_alert(fmt, ...)   printk(KERN_ALERT   pr_fmt(fmt), ##__VA_ARGS__)
#define pr_crit(fmt, ...)    printk(KERN_CRIT    pr_fmt(fmt), ##__VA_ARGS__)
#define pr_err(fmt, ...)     printk(KERN_ERR     pr_fmt(fmt), ##__VA_ARGS__)
#define pr_warn(fmt, ...)    printk(KERN_WARNING pr_fmt(fmt), ##__VA_ARGS__)
#define pr_warning(fmt, ...) printk(KERN_WARNING pr_fmt(fmt), ##__VA_ARGS__)
#define pr_notice(fmt, ...)  printk(KERN_NOTICE  pr_fmt(fmt), ##__VA_ARGS__)
#define pr_info(fmt, ...)    printk(KERN_INFO    pr_fmt(fmt), ##__VA_ARGS__)
#define pr_cont(fmt, ...)    printk(KERN_CONT    pr_fmt(fmt), ##__VA_ARGS__)
/* pr_debug is a no-op unless DEBUG (Linux default); keeps vring/DRM debug chatter off the
 * console — the lifted virtio_ring.c calls pr_debug on every buffer add/get. */
#ifdef DEBUG
#define pr_debug(fmt, ...)   printk(KERN_DEBUG   pr_fmt(fmt), ##__VA_ARGS__)
#else
#define pr_debug(fmt, ...)   do { } while (0)
#endif
#define pr_info_once(fmt, ...)  printk(KERN_INFO pr_fmt(fmt), ##__VA_ARGS__)
#define pr_err_once(fmt, ...)   printk(KERN_ERR  pr_fmt(fmt), ##__VA_ARGS__)

#endif /* _LINUXKPI_LINUX_PRINTK_H */

#ifndef _LKPI_PRINTK_EXTRA
#define _LKPI_PRINTK_EXTRA
#include <linux/types.h>
char *kasprintf(unsigned gfp, const char *fmt, ...);
char *kvasprintf(unsigned gfp, const char *fmt, va_list ap);
#endif

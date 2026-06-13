/* syslog.h — system logging. Stub for ports (busybox udhcp); NanOS routes these to stderr via the
 * libc-glue impl in posixstubs.c. */
#ifndef _NANOS_SYSLOG_H
#define _NANOS_SYSLOG_H
#include <stdarg.h>
#define LOG_EMERG 0
#define LOG_ALERT 1
#define LOG_CRIT 2
#define LOG_ERR 3
#define LOG_WARNING 4
#define LOG_NOTICE 5
#define LOG_INFO 6
#define LOG_DEBUG 7
#define LOG_PRIMASK 0x07
#define LOG_PRI(p) ((p) & LOG_PRIMASK)
#define LOG_PID    0x01
#define LOG_CONS   0x02
#define LOG_ODELAY 0x04
#define LOG_NDELAY 0x08
#define LOG_NOWAIT 0x10
#define LOG_KERN   (0<<3)
#define LOG_USER   (1<<3)
#define LOG_MAIL   (2<<3)
#define LOG_DAEMON (3<<3)
#define LOG_LOCAL0 (16<<3)
#define LOG_MASK(pri) (1 << (pri))
#define LOG_UPTO(pri) ((1 << ((pri)+1)) - 1)
#ifdef __cplusplus
extern "C" {
#endif
void openlog(const char* ident, int option, int facility);
void syslog(int priority, const char* format, ...);
void vsyslog(int priority, const char* format, va_list ap);
void closelog(void);
int  setlogmask(int mask);
#ifdef __cplusplus
}
#endif
#endif

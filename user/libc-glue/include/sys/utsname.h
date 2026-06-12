/*
 * sys/utsname.h — uname(2) surface (Linux layout). NanOS identifies itself through the kernel
 * version string; this struct lets apps query it.
 */
#ifndef _SYS_UTSNAME_H
#define _SYS_UTSNAME_H

#ifdef __cplusplus
extern "C" {
#endif

#define _UTSNAME_LENGTH 65

struct utsname {
	char sysname[_UTSNAME_LENGTH];
	char nodename[_UTSNAME_LENGTH];
	char release[_UTSNAME_LENGTH];
	char version[_UTSNAME_LENGTH];
	char machine[_UTSNAME_LENGTH];
	char domainname[_UTSNAME_LENGTH];
};

int uname(struct utsname* buf);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_UTSNAME_H */

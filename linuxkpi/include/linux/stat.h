/* linuxkpi/include/linux/stat.h — sysfs mode bits (i915 debugfs/sysfs attributes). */
#ifndef _LINUXKPI_LINUX_STAT_H
#define _LINUXKPI_LINUX_STAT_H
#ifndef S_IRUSR
#define S_IRWXU 0700
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXG 0070
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IROTH 0004
#define S_IWOTH 0002
#endif
#ifndef S_IRUGO
#define S_IRUGO (S_IRUSR|S_IRGRP|S_IROTH)
#define S_IWUSR_IRUGO (S_IWUSR|S_IRUGO)
#define S_IRUGO_IWUSR (S_IWUSR|S_IRUGO)
#endif
#endif

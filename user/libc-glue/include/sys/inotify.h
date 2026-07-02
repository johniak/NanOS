/* sys/inotify.h — stub (picolibc ships none; toybox lib/portability.c includes it). NanOS has
 * no inotify; the calls return -1/ENOSYS. Decls + flags only, for compilation. */
#ifndef _SYS_INOTIFY_H
#define _SYS_INOTIFY_H
#include <stdint.h>
struct inotify_event { int wd; uint32_t mask, cookie, len; char name[]; };
#define IN_ACCESS 0x001
#define IN_MODIFY 0x002
#define IN_CREATE 0x100
#define IN_DELETE 0x200
#define IN_MOVED_FROM 0x40
#define IN_MOVED_TO 0x80
#define IN_CLOEXEC 02000000
#define IN_NONBLOCK 04000
/* additional event bits (standard Linux values) used by Mesa's disk-cache dir watcher */
#define IN_CLOSE_WRITE   0x00000008
#define IN_CLOSE_NOWRITE 0x00000010
#define IN_DELETE_SELF   0x00000400
#define IN_MOVE_SELF     0x00000800
#define IN_UNMOUNT       0x00002000
#define IN_Q_OVERFLOW    0x00004000
#define IN_IGNORED       0x00008000
#define IN_ONLYDIR       0x01000000
#define IN_ISDIR         0x40000000
int inotify_init(void);
int inotify_init1(int flags);
int inotify_add_watch(int fd, const char* path, uint32_t mask);
int inotify_rm_watch(int fd, int wd);
#endif

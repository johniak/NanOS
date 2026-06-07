/*
 * sys/sysmacros.h — minimal device-number macros (picolibc omits this header).
 * NanOS has no real device numbers; ls only references these for completeness.
 */
#ifndef _NX_SYSMACROS_H
#define _NX_SYSMACROS_H

#define major(dev)    ((int) (((dev) >> 8) & 0xff))
#define minor(dev)    ((int) ((dev) & 0xff))
#define makedev(a, b) (((a) << 8) | (b))

#endif /* _NX_SYSMACROS_H */

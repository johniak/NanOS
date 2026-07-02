/*
 * sys/ioccom.h — ioctl-number encoding for userspace.
 *
 * The DRM uapi headers (drm.h) that libdrm/Mesa bundle select their ioctl-number macros by
 * platform: __KERNEL__/__linux__ -> <asm/ioctl.h>; otherwise "one of the BSDs" -> <sys/ioccom.h>.
 * The x86_64-nanos cross-gcc defines neither __KERNEL__ nor __linux__, so drm.h takes the BSD
 * branch and includes THIS header. NanOS speaks the LINUX ioctl ABI, and the kernel-side DRM core
 * decodes ioctl numbers with the standard Linux _IOC layout — so we MUST provide that same layout
 * here (not the real BSD one), or every DRM ioctl number would mismatch. Also define __user
 * (empty), which the BSD branch does not.
 */
#ifndef _NANOS_ASM_IOCTL_H
#define _NANOS_ASM_IOCTL_H

#define _IOC_NRBITS    8
#define _IOC_TYPEBITS  8
#define _IOC_SIZEBITS  14
#define _IOC_DIRBITS   2

#define _IOC_NRMASK    ((1 << _IOC_NRBITS) - 1)
#define _IOC_TYPEMASK  ((1 << _IOC_TYPEBITS) - 1)
#define _IOC_SIZEMASK  ((1 << _IOC_SIZEBITS) - 1)
#define _IOC_DIRMASK   ((1 << _IOC_DIRBITS) - 1)

#define _IOC_NRSHIFT   0
#define _IOC_TYPESHIFT (_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT  (_IOC_SIZESHIFT + _IOC_SIZEBITS)

#define _IOC_NONE  0U
#define _IOC_WRITE 1U
#define _IOC_READ  2U

#define _IOC(dir, type, nr, size) \
	(((dir)  << _IOC_DIRSHIFT)  | ((type) << _IOC_TYPESHIFT) | \
	 ((nr)   << _IOC_NRSHIFT)   | ((size) << _IOC_SIZESHIFT))

#define _IOC_TYPECHECK(t) (sizeof(t))

#define _IO(type, nr)            _IOC(_IOC_NONE, (type), (nr), 0)
#define _IOR(type, nr, size)     _IOC(_IOC_READ, (type), (nr), (_IOC_TYPECHECK(size)))
#define _IOW(type, nr, size)     _IOC(_IOC_WRITE, (type), (nr), (_IOC_TYPECHECK(size)))
#define _IOWR(type, nr, size)    _IOC(_IOC_READ | _IOC_WRITE, (type), (nr), (_IOC_TYPECHECK(size)))

#define _IOC_DIR(nr)   (((nr) >> _IOC_DIRSHIFT) & _IOC_DIRMASK)
#define _IOC_TYPE(nr)  (((nr) >> _IOC_TYPESHIFT) & _IOC_TYPEMASK)
#define _IOC_NR(nr)    (((nr) >> _IOC_NRSHIFT) & _IOC_NRMASK)
#define _IOC_SIZE(nr)  (((nr) >> _IOC_SIZESHIFT) & _IOC_SIZEMASK)

/* BSD-style ioctl direction names. libdrm's xf86drm.h (BSD branch) feeds these as the `dir`
 * argument to _IOC(dir, group, nr, size). Alias them to the LINUX direction VALUES so the
 * BSD-branch DRM_IOC macros produce the same Linux ioctl numbers as drm.h's _IOWR/_IOR/_IOW. */
#ifndef IOC_VOID
#define IOC_VOID  _IOC_NONE
#define IOC_OUT   _IOC_READ
#define IOC_IN    _IOC_WRITE
#define IOC_INOUT (IOC_IN | IOC_OUT)
#endif

#ifndef __user
#define __user
#endif

#endif /* _NANOS_ASM_IOCTL_H */

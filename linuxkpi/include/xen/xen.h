/*
 * linuxkpi/include/xen/xen.h — NanOS is never a Xen guest, so all Xen predicates are false.
 */
#ifndef _LINUXKPI_XEN_XEN_H
#define _LINUXKPI_XEN_XEN_H

#define xen_domain()          0
#define xen_pv_domain()       0
#define xen_hvm_domain()      0
#define xen_initial_domain()  0
#define xen_pvh_domain()      0

#endif /* _LINUXKPI_XEN_XEN_H */

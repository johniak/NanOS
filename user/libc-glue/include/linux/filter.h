/* linux/filter.h — classic BPF structs (busybox udhcpc attaches a socket filter). NanOS has no
 * in-kernel BPF; SO_ATTACH_FILTER is a no-op, so udhcpc filters in userspace. Structs only. */
#ifndef _NANOS_LINUX_FILTER_H
#define _NANOS_LINUX_FILTER_H
#include <stdint.h>
struct sock_filter { uint16_t code; uint8_t jt; uint8_t jf; uint32_t k; };
struct sock_fprog  { uint16_t len; struct sock_filter* filter; };
#endif

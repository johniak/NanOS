/* linuxkpi/include/linux/relay.h — GuC log relay channel; NanOS has no relayfs. Inert. */
#ifndef _LKPI_LINUX_RELAY_H
#define _LKPI_LINUX_RELAY_H
struct rchan; struct rchan_callbacks; struct dentry;
/* No relayfs backing: the channel is always "not full" (writes are dropped, never blocked).
 * Takes void* so it matches whatever rchan_buf pointer the caller passes without a type clash. */
static inline int relay_buf_full(void *buf){ (void)buf; return 0; }
#endif

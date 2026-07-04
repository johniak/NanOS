/* linuxkpi/include/linux/relay.h — GuC log relay channel; NanOS has no relayfs. Inert. */
#ifndef _LKPI_LINUX_RELAY_H
#define _LKPI_LINUX_RELAY_H
struct rchan_buf; struct dentry;
struct rchan { void *private_data; size_t subbuf_size; size_t n_subbufs; };
/* relay channel callbacks (GuC log). Inert — the shim has no relayfs, so these are never invoked. */
struct rchan_callbacks {
	int (*subbuf_start)(struct rchan_buf *buf, void *subbuf, void *prev_subbuf, unsigned long prev_padding);
	struct dentry *(*create_buf_file)(const char *filename, struct dentry *parent, unsigned short mode, struct rchan_buf *buf, int *is_global);
	int (*remove_buf_file)(struct dentry *dentry);
};
static inline struct rchan *relay_open(const char *base, struct dentry *parent, unsigned long subbuf_size, unsigned long n_subbufs, const struct rchan_callbacks *cb, void *priv){ (void)base;(void)parent;(void)subbuf_size;(void)n_subbufs;(void)cb;(void)priv; return 0; }
static inline void relay_close(struct rchan *chan){ (void)chan; }
static inline void relay_flush(struct rchan *chan){ (void)chan; }
static inline size_t relay_switch_subbuf(struct rchan_buf *buf, size_t length){ (void)buf;(void)length; return 0; }
static inline void *relay_reserve(struct rchan *chan, size_t length){ (void)chan;(void)length; return 0; }
/* No relayfs backing: the channel is always "not full" (writes are dropped, never blocked).
 * Takes void* so it matches whatever rchan_buf pointer the caller passes without a type clash. */
static inline int relay_buf_full(void *buf){ (void)buf; return 0; }
struct file_operations;
extern const struct file_operations relay_file_operations;   /* GuC-log relay node fops (inert) */
#endif

#ifndef _LKPI_EVENTFD_H
#define _LKPI_EVENTFD_H
struct eventfd_ctx;
static inline void eventfd_ctx_put(struct eventfd_ctx *c){ (void)c; }
static inline void eventfd_signal(struct eventfd_ctx *c){ (void)c; }
static inline struct eventfd_ctx *eventfd_ctx_fdget(int fd){ (void)fd; return 0; }
#endif

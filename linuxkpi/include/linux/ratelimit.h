#ifndef _LKPI_RATELIMIT_H
#define _LKPI_RATELIMIT_H
struct ratelimit_state { int interval; int burst; };
#define DEFINE_RATELIMIT_STATE(name, intv, brst) struct ratelimit_state name = { (intv), (brst) }
#define RATELIMIT_STATE_INIT(name, intv, brst) { (intv), (brst) }
static inline int __ratelimit(struct ratelimit_state *rs){ (void)rs; return 1; }
#define ratelimit_state_init(rs, i, b) do { (rs)->interval=(i); (rs)->burst=(b); } while(0)
#endif

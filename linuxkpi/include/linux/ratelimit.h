#ifndef _LKPI_RATELIMIT_H
#define _LKPI_RATELIMIT_H
#define DEFAULT_RATELIMIT_INTERVAL (5*100)
#define DEFAULT_RATELIMIT_BURST 10
struct ratelimit_state { int interval; int burst; int printed; int missed; unsigned long begin; unsigned long flags; };
#define DEFINE_RATELIMIT_STATE(name, intv, brst) struct ratelimit_state name = { (intv), (brst) }
#define RATELIMIT_STATE_INIT(name, intv, brst) { (intv), (brst) }
static inline int __ratelimit(struct ratelimit_state *rs){ (void)rs; return 1; }
#define ratelimit_state_init(rs, i, b) do { (rs)->interval=(i); (rs)->burst=(b); } while(0)
#define RATELIMIT_MSG_ON_RELEASE (1<<0)
static inline void ratelimit_set_flags(struct ratelimit_state *rs, unsigned long flags){ if(rs) rs->flags = flags; }
static inline void ratelimit_state_exit(struct ratelimit_state *rs){ (void)rs; }
#endif

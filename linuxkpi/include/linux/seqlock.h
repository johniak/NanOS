#ifndef _LKPI_SEQLOCK_H
#define _LKPI_SEQLOCK_H
#include <linux/spinlock.h>
typedef struct { unsigned sequence; spinlock_t lock; } seqlock_t;
typedef struct { unsigned sequence; } seqcount_t;
#define seqlock_init(s) do { (s)->sequence=0; spin_lock_init(&(s)->lock); } while(0)
#define seqcount_init(s) do { (s)->sequence=0; } while(0)
static inline unsigned read_seqbegin(const seqlock_t *s){ return s->sequence; }
static inline unsigned read_seqretry(const seqlock_t *s, unsigned start){ (void)start; (void)s; return 0; }
static inline void write_seqlock(seqlock_t *s){ s->sequence++; }
static inline void write_sequnlock(seqlock_t *s){ s->sequence++; }
static inline void write_seqlock_irqsave(seqlock_t *s, unsigned long f){ (void)f; s->sequence++; }
static inline void write_sequnlock_irqrestore(seqlock_t *s, unsigned long f){ (void)f; s->sequence++; }
#define write_seqlock_irqsave(s,f) do{ (f)=0; (s)->sequence++; }while(0)
#define write_sequnlock_irqrestore(s,f) do{ (void)(f); (s)->sequence++; }while(0)
static inline unsigned raw_read_seqcount(const seqcount_t *s){ return s->sequence; }
static inline unsigned read_seqcount_begin(const seqcount_t *s){ return s->sequence; }
static inline int read_seqcount_retry(const seqcount_t *s, unsigned start){ (void)s;(void)start; return 0; }
static inline void write_seqcount_begin(seqcount_t *s){ s->sequence++; }
static inline void write_seqcount_end(seqcount_t *s){ s->sequence++; }
/* Invalidate in-flight readers (i915 intel_tlb after a full TLB flush): bump the counter so any
 * outstanding read section would retry. */
static inline void write_seqcount_invalidate(seqcount_t *s){ s->sequence += 2; }
/* A seqcount associated with a mutex. With lockdep off the mutex is only a lockdep annotation and
 * carries no runtime state, so a mutex-seqcount IS a bare seqcount_t here — every seqcount op above
 * applies to it directly. seqcount_mutex_init just zeroes the counter and drops the lock argument. */
typedef seqcount_t seqcount_mutex_t;
#define seqcount_mutex_init(s, m) do { (s)->sequence = 0; (void)(m); } while (0)
/* Read the raw sequence value of any seqcount variant (Linux's seqprop accessor). All our seqcount
 * types carry a bare `sequence`, so this is a direct field read. i915 intel_tlb uses it. */
#define seqprop_sequence(s) ((s)->sequence)
#endif

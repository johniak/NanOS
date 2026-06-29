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
#endif

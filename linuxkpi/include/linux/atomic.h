/*
 * linuxkpi/include/linux/atomic.h — atomics for the shim via GCC __atomic builtins.
 */
#ifndef _LINUXKPI_LINUX_ATOMIC_H
#define _LINUXKPI_LINUX_ATOMIC_H

#include <linux/types.h>
#include <asm/barrier.h>

typedef struct { int counter; } atomic_t;
typedef struct { long counter; } atomic64_t;

#define ATOMIC_INIT(i)   { (i) }
#define ATOMIC64_INIT(i) { (i) }

static inline int  atomic_read(const atomic_t *v) { return __atomic_load_n(&v->counter, __ATOMIC_RELAXED); }
static inline void atomic_set(atomic_t *v, int i) { __atomic_store_n(&v->counter, i, __ATOMIC_RELAXED); }
static inline int  atomic_add_return(int i, atomic_t *v) { return __atomic_add_fetch(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline int  atomic_sub_return(int i, atomic_t *v) { return __atomic_sub_fetch(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline void atomic_add(int i, atomic_t *v) { (void)atomic_add_return(i, v); }
static inline void atomic_sub(int i, atomic_t *v) { (void)atomic_sub_return(i, v); }
static inline void atomic_inc(atomic_t *v) { (void)atomic_add_return(1, v); }
static inline void atomic_dec(atomic_t *v) { (void)atomic_sub_return(1, v); }
static inline int  atomic_inc_return(atomic_t *v) { return atomic_add_return(1, v); }
static inline int  atomic_dec_return(atomic_t *v) { return atomic_sub_return(1, v); }
static inline int  atomic_dec_and_test(atomic_t *v) { return atomic_sub_return(1, v) == 0; }
static inline int  atomic_inc_and_test(atomic_t *v) { return atomic_add_return(1, v) == 0; }
static inline int  atomic_sub_and_test(int i, atomic_t *v) { return atomic_sub_return(i, v) == 0; }
static inline int  atomic_fetch_add(int i, atomic_t *v) { return __atomic_fetch_add(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline int  atomic_fetch_sub(int i, atomic_t *v) { return __atomic_fetch_sub(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline int  atomic_cmpxchg(atomic_t *v, int old, int n) {
	__atomic_compare_exchange_n(&v->counter, &old, n, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return old;
}
static inline int  atomic_xchg(atomic_t *v, int n) { return __atomic_exchange_n(&v->counter, n, __ATOMIC_SEQ_CST); }
static inline int  atomic_add_unless(atomic_t *v, int a, int u) {
	int c = atomic_read(v);
	while (c != u) { if (__atomic_compare_exchange_n(&v->counter, &c, c + a, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return 1; }
	return 0;
}

static inline long atomic64_read(const atomic64_t *v) { return __atomic_load_n(&v->counter, __ATOMIC_RELAXED); }
static inline void atomic64_set(atomic64_t *v, long i) { __atomic_store_n(&v->counter, i, __ATOMIC_RELAXED); }
static inline long atomic64_add_return(long i, atomic64_t *v) { return __atomic_add_fetch(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline long atomic64_inc_return(atomic64_t *v) { return atomic64_add_return(1, v); }
static inline void atomic64_inc(atomic64_t *v) { (void)atomic64_add_return(1, v); }

#define cmpxchg(ptr, o, n) __sync_val_compare_and_swap((ptr), (o), (n))
#define xchg(ptr, n) __atomic_exchange_n((ptr), (n), __ATOMIC_SEQ_CST)

#endif /* _LINUXKPI_LINUX_ATOMIC_H */

#ifndef _LKPI_ATOMIC_X
#define _LKPI_ATOMIC_X
static inline void atomic64_add(long i, atomic64_t *v){ (void)atomic64_add_return(i,v); }
static inline long atomic64_sub_return(long i, atomic64_t *v){ return __atomic_sub_fetch(&v->counter,i,__ATOMIC_SEQ_CST); }
typedef struct { atomic_t r; } refcount_t;
static inline void refcount_set(refcount_t *r, int n){ atomic_set(&r->r,n); }
static inline int refcount_read(const refcount_t *r){ return atomic_read(&r->r); }
static inline void refcount_inc(refcount_t *r){ atomic_inc(&r->r); }
static inline int refcount_dec_and_test(refcount_t *r){ return atomic_dec_and_test(&r->r); }
static inline int refcount_inc_not_zero(refcount_t *r){ return atomic_add_unless(&r->r,1,0); }
#endif

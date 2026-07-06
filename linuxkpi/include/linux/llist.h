#ifndef _LKPI_LLIST_H
#define _LKPI_LLIST_H
#include <linux/types.h>
#include <linux/atomic.h>
struct llist_node { struct llist_node *next; };
struct llist_head { struct llist_node *first; };
#define LLIST_HEAD_INIT(name) { 0 }
#define LLIST_HEAD(name) struct llist_head name = LLIST_HEAD_INIT(name)
static inline void init_llist_head(struct llist_head *h){ h->first=0; }
static inline bool llist_empty(const struct llist_head *h){ return h->first==0; }
/* An llist is contracted to be lock-free-safe from ANY context (thread, IRQ, tasklet) — i915's
 * b->signaled_requests is fed by irq_signal_request() from both a hardirq frame and a thread. On a
 * real CPU that is a cmpxchg loop; the shim has no SMP but is single-core-preemptible via the inline
 * IRQ/irq_work model, so an MSI landing between the read of h->first and the store would corrupt the
 * chain. Make each read-modify-write atomic w.r.t. local interrupt delivery (save/cli/restore); zero
 * cost on the host doctest build. (Fable audit, window 3.) */
#ifdef NANOS_HOST_TEST
#define __LKPI_LLIST_ATOMIC_ENTER(f) do { (f) = 0; } while (0)
#define __LKPI_LLIST_ATOMIC_LEAVE(f) do { (void)(f); } while (0)
#else
#define __LKPI_LLIST_ATOMIC_ENTER(f) __asm__ __volatile__("pushfq; popq %0; cli" : "=r"(f) : : "memory")
#define __LKPI_LLIST_ATOMIC_LEAVE(f) __asm__ __volatile__("pushq %0; popfq" : : "r"(f) : "memory", "cc")
#endif
static inline bool llist_add(struct llist_node *n, struct llist_head *h){
  unsigned long __f; bool __e; __LKPI_LLIST_ATOMIC_ENTER(__f);
  { struct llist_node *first=h->first; n->next=first; h->first=n; __e=(first==0); }
  __LKPI_LLIST_ATOMIC_LEAVE(__f); return __e; }
static inline struct llist_node *llist_del_all(struct llist_head *h){
  unsigned long __f; struct llist_node *first; __LKPI_LLIST_ATOMIC_ENTER(__f);
  first=h->first; h->first=0; __LKPI_LLIST_ATOMIC_LEAVE(__f); return first; }
/* push a [first..last] chain (last->next already terminated) onto the head; returns was-empty. */
static inline bool llist_add_batch(struct llist_node *first, struct llist_node *last, struct llist_head *h){
  unsigned long __f; bool __e; __LKPI_LLIST_ATOMIC_ENTER(__f);
  { struct llist_node *old=h->first; last->next=old; h->first=first; __e=(old==0); }
  __LKPI_LLIST_ATOMIC_LEAVE(__f); return __e; }
static inline struct llist_node *llist_del_first(struct llist_head *h){
  unsigned long __f; struct llist_node *first; __LKPI_LLIST_ATOMIC_ENTER(__f);
  first=h->first; if(first) h->first=first->next; __LKPI_LLIST_ATOMIC_LEAVE(__f); return first; }
#define llist_entry(ptr,type,member) container_of(ptr,type,member)
/* The NULL-terminated llist walk ends when the current NODE pointer is NULL — i.e. when the
 * iterator, reconstructed from that node via container_of, has its member back at address 0.
 * Testing `&pos->member != 0` as a POINTER is undefined behavior: GCC (esp. at -O2, which i915
 * requires) assumes the address of a struct member is never NULL and DELETES the check, turning
 * the loop into an unterminated do-while that walks container_of(NULL) and #PFs. Upstream defeats
 * this with member_address_is_nonnull(), which forces the comparison into integer (uintptr_t)
 * arithmetic the optimizer must honor. Mirror it exactly. */
#define member_address_is_nonnull(ptr,member) \
  ((uintptr_t)(ptr) + __builtin_offsetof(__typeof__(*(ptr)),member) != 0)
#define llist_for_each(pos,node) for(pos=(node);pos;pos=pos->next)
#define llist_for_each_entry(pos,node,member) \
  for(pos=llist_entry((node),__typeof__(*pos),member); \
      member_address_is_nonnull(pos,member); \
      pos=llist_entry(pos->member.next,__typeof__(*pos),member))
#define llist_for_each_safe(pos,n,node) for(pos=(node);pos&&((n=pos->next),1);pos=n)
#define llist_for_each_entry_safe(pos,n,node,member) \
  for(pos=llist_entry((node),__typeof__(*pos),member); \
      member_address_is_nonnull(pos,member)&&((n=llist_entry(pos->member.next,__typeof__(*pos),member)),1); \
      pos=n)
#endif

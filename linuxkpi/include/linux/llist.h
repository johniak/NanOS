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
static inline bool llist_add(struct llist_node *n, struct llist_head *h){
  struct llist_node *f=h->first; n->next=f; h->first=n; return f==0; }
static inline struct llist_node *llist_del_all(struct llist_head *h){
  struct llist_node *f=h->first; h->first=0; return f; }
/* push a [first..last] chain (last->next already terminated) onto the head; returns was-empty. */
static inline bool llist_add_batch(struct llist_node *first, struct llist_node *last, struct llist_head *h){
  struct llist_node *f=h->first; last->next=f; h->first=first; return f==0; }
static inline struct llist_node *llist_del_first(struct llist_head *h){
  struct llist_node *f=h->first; if(f) h->first=f->next; return f; }
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

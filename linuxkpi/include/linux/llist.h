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
#define llist_entry(ptr,type,member) container_of(ptr,type,member)
#define llist_for_each_safe(pos,n,node) for(pos=(node);pos&&((n=pos->next),1);pos=n)
#define llist_for_each_entry_safe(pos,n,node,member) \
  for(pos=llist_entry((node),__typeof__(*pos),member); \
      &pos->member!=0&&((n=llist_entry(pos->member.next,__typeof__(*pos),member)),1); \
      pos=n)
#endif

/*
 * linuxkpi/include/linux/list.h — the classic Linux doubly-linked list + hlist, the subset
 * the vendored virtio/DRM core uses.
 */
#ifndef _LINUXKPI_LINUX_LIST_H
#define _LINUXKPI_LINUX_LIST_H

#include <linux/types.h>
#include <linux/kernel.h>

struct list_head { struct list_head *next, *prev; };
struct hlist_head { struct hlist_node *first; };
struct hlist_node { struct hlist_node *next, **pprev; };

#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head *list) { list->next = list; list->prev = list; }

static inline void __list_add(struct list_head *n, struct list_head *prev, struct list_head *next) {
	next->prev = n; n->next = next; n->prev = prev; prev->next = n;
}
static inline void list_add(struct list_head *n, struct list_head *head) { __list_add(n, head, head->next); }
static inline void list_add_tail(struct list_head *n, struct list_head *head) { __list_add(n, head->prev, head); }
static inline void __list_del(struct list_head *prev, struct list_head *next) { next->prev = prev; prev->next = next; }
static inline void list_del(struct list_head *entry) { __list_del(entry->prev, entry->next); entry->next = 0; entry->prev = 0; }
static inline void list_del_init(struct list_head *entry) { __list_del(entry->prev, entry->next); INIT_LIST_HEAD(entry); }
static inline void list_move(struct list_head *list, struct list_head *head) { __list_del(list->prev, list->next); list_add(list, head); }
static inline void list_move_tail(struct list_head *list, struct list_head *head) { __list_del(list->prev, list->next); list_add_tail(list, head); }
static inline int  list_empty(const struct list_head *head) { return head->next == head; }
static inline void list_replace(struct list_head *old, struct list_head *n) {
	n->next = old->next; n->next->prev = n; n->prev = old->prev; n->prev->next = n;
}
static inline int list_is_last(const struct list_head *list, const struct list_head *head) { return list->next == head; }
static inline int list_is_first(const struct list_head *list, const struct list_head *head) { return list->prev == head; }

#define list_entry(ptr, type, member)        container_of(ptr, type, member)
#define list_first_entry(ptr, type, member)  list_entry((ptr)->next, type, member)
#define list_last_entry(ptr, type, member)   list_entry((ptr)->prev, type, member)
#define list_next_entry(pos, member)         list_entry((pos)->member.next, __typeof__(*(pos)), member)
#define list_prev_entry(pos, member)         list_entry((pos)->member.prev, __typeof__(*(pos)), member)
#define list_first_entry_or_null(ptr, type, member) \
	({ struct list_head *h__ = (ptr); struct list_head *p__ = h__->next; p__ != h__ ? list_entry(p__, type, member) : (type *)0; })

#define list_for_each(pos, head) for (pos = (head)->next; pos != (head); pos = pos->next)
#define list_for_each_safe(pos, n, head) \
	for (pos = (head)->next, n = pos->next; pos != (head); pos = n, n = pos->next)
#define list_for_each_entry(pos, head, member) \
	for (pos = list_first_entry(head, __typeof__(*pos), member); \
	     &pos->member != (head); pos = list_next_entry(pos, member))
#define list_for_each_entry_safe(pos, n, head, member) \
	for (pos = list_first_entry(head, __typeof__(*pos), member), \
	     n = list_next_entry(pos, member); &pos->member != (head); \
	     pos = n, n = list_next_entry(n, member))
#define list_for_each_entry_reverse(pos, head, member) \
	for (pos = list_last_entry(head, __typeof__(*pos), member); \
	     &pos->member != (head); pos = list_prev_entry(pos, member))

/* hlist */
#define HLIST_HEAD_INIT { .first = 0 }
#define HLIST_HEAD(name) struct hlist_head name = { .first = 0 }
static inline void INIT_HLIST_NODE(struct hlist_node *h) { h->next = 0; h->pprev = 0; }
static inline void INIT_HLIST_HEAD(struct hlist_head *h) { h->first = 0; }
static inline int  hlist_empty(const struct hlist_head *h) { return !h->first; }
static inline void hlist_add_head(struct hlist_node *n, struct hlist_head *h) {
	n->next = h->first; if (h->first) h->first->pprev = &n->next; h->first = n; n->pprev = &h->first;
}
static inline void hlist_del(struct hlist_node *n) {
	if (n->next) n->next->pprev = n->pprev; if (n->pprev) *n->pprev = n->next;
}
static inline void hlist_del_init(struct hlist_node *n) { if (n->pprev) { hlist_del(n); INIT_HLIST_NODE(n); } }
#define hlist_entry(ptr, type, member) container_of(ptr, type, member)
#define hlist_entry_safe(ptr, type, member) ({ __typeof__(ptr) ____ptr = (ptr); ____ptr ? hlist_entry(____ptr, type, member) : (type *)0; })
#define hlist_for_each_entry(pos, head, member) \
	for (pos = hlist_entry_safe((head)->first, __typeof__(*(pos)), member); \
	     pos; pos = hlist_entry_safe((pos)->member.next, __typeof__(*(pos)), member))
#define hlist_for_each_entry_safe(pos, n, head, member) \
	for (pos = hlist_entry_safe((head)->first, __typeof__(*(pos)), member); \
	     pos && ({ n = pos->member.next; 1; }); \
	     pos = hlist_entry_safe(n, __typeof__(*(pos)), member))

#endif /* _LINUXKPI_LINUX_LIST_H */

#ifndef _LKPI_LIST_EXTRA
#define _LKPI_LIST_EXTRA
static inline void list_splice_tail(struct list_head *list, struct list_head *head){
  if(!list_empty(list)){ struct list_head *f=list->next,*l=list->prev,*p=head->prev;
    p->next=f; f->prev=p; l->next=head; head->prev=l; } }
static inline void list_splice_tail_init(struct list_head *list, struct list_head *head){ list_splice_tail(list,head); INIT_LIST_HEAD(list); }
static inline int list_is_singular(const struct list_head *head){ return !list_empty(head) && head->next == head->prev; }
/* Move the sublist [first..last] (already a contiguous run inside its list) to head's tail.
 * ttm_resource uses it to bulk-requeue LRU entries. */
static inline void list_bulk_move_tail(struct list_head *head, struct list_head *first, struct list_head *last){
  first->prev->next = last->next; last->next->prev = first->prev;
  last->next = head; first->prev = head->prev;
  head->prev->next = first; head->prev = last;
}
#endif

#ifndef _LKPI_LIST_SORT
#define _LKPI_LIST_SORT
static inline void list_splice(const struct list_head *list, struct list_head *head){
  if(!list_empty((struct list_head*)list)){ struct list_head *f=list->next,*l=list->prev,*at=head->next;
    head->next=f; f->prev=head; l->next=at; at->prev=l; } }
static inline void list_splice_init(struct list_head *list, struct list_head *head){ list_splice(list,head); INIT_LIST_HEAD(list); }
typedef int __attribute__((nonnull(2,3))) (*list_cmp_func_t)(void *priv, const struct list_head *a, const struct list_head *b);
void list_sort(void *priv, struct list_head *head, list_cmp_func_t cmp);
#endif

#ifndef _LKPI_LIST_X2
#define _LKPI_LIST_X2
static inline void __list_del_entry(struct list_head *entry){ __list_del(entry->prev, entry->next); }
#define list_for_each_entry_continue(pos, head, member) \
	for (pos = list_next_entry(pos, member); &pos->member != (head); pos = list_next_entry(pos, member))
#define list_for_each_entry_from(pos, head, member) \
	for (; &pos->member != (head); pos = list_next_entry(pos, member))
#define list_for_each_entry_safe_from(pos, n, head, member) \
	for (n = list_next_entry(pos, member); &pos->member != (head); pos = n, n = list_next_entry(n, member))
#define list_for_each_entry_from_reverse(pos, head, member) \
	for (; &pos->member != (head); pos = list_prev_entry(pos, member))
#endif

#ifndef _LKPI_LIST_X3
#define _LKPI_LIST_X3
/* Empty-check safe against concurrent list_del (careful): both ends must point back at head. */
static inline int list_empty_careful(const struct list_head *head){ struct list_head *next=head->next; return (next==head) && (next==head->prev); }
#define list_for_each_entry_safe_reverse(pos, n, head, member) \
	for (pos = list_last_entry(head, __typeof__(*pos), member), n = list_prev_entry(pos, member); \
	     &pos->member != (head); pos = n, n = list_prev_entry(n, member))
/* Re-seat the safe-iteration cursor after the caller moved `pos` (i915 execlists dequeue). */
#define list_safe_reset_next(pos, n, member) \
	(n) = list_next_entry(pos, member)
/* RCU list ops degrade to the plain ops: the shim's deferred-preemption RCU has no separate publish
 * barrier requirement for a kernel-mode reader (see [[nanos-gpu-plans]] synchronize_rcu note). */
#define list_add_rcu(new, head)      list_add(new, head)
#define list_add_tail_rcu(new, head) list_add_tail(new, head)
/* list_del_rcu is the ONE RCU op that CANNOT degrade to list_del: it must leave entry->next intact.
 * A reader can be mid-traversal parked on the very node being deleted and still advances via
 * pos->next (i915 gt/intel_breadcrumbs.c signal_irq_work deletes the current rq inside
 * list_for_each_entry_rcu(&ce->signals) then the loop reads rq->signal_link.next to continue).
 * The plain list_del zeroes entry->next → the next iteration does container_of(NULL,...) and #PFs.
 * Unlink from both neighbours, poison only ->prev, leave ->next pointing onward (upstream semantics). */
static inline void list_del_rcu(struct list_head *entry) { __list_del(entry->prev, entry->next); entry->prev = 0; }
#define list_for_each_entry_rcu(pos, head, member, ...) list_for_each_entry(pos, head, member)
#define list_first_or_null_rcu(ptr, type, member) \
	({ struct list_head *__h = (ptr); __h->next != __h ? list_entry(__h->next, type, member) : (type*)0; })
#endif

#ifndef _LKPI_LIST_PREV
#define _LKPI_LIST_PREV
#define list_for_each_prev(pos, head) for ((pos) = (head)->prev; (pos) != (head); (pos) = (pos)->prev)
/* lockless list walk: the deferred-preemption reader can't be preempted mid-walk, so plain iteration. */
#define list_for_each_entry_lockless(pos, head, member) list_for_each_entry(pos, head, member)
static inline unsigned long list_count_nodes(struct list_head *head){ unsigned long n=0; struct list_head *p; for(p=(head)->next; p!=(head); p=p->next) n++; return n; }
#define list_for_each_prev_safe(pos, n, head) \
	for ((pos) = (head)->prev, (n) = (pos)->prev; (pos) != (head); (pos) = (n), (n) = (pos)->prev)
#endif

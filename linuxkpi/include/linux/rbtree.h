#ifndef _LKPI_RBTREE_H
#define _LKPI_RBTREE_H
#include <linux/types.h>
#include <linux/kernel.h>
struct rb_node { unsigned long __rb_parent_color; struct rb_node *rb_right, *rb_left; } __attribute__((aligned(sizeof(long))));
struct rb_root { struct rb_node *rb_node; };
struct rb_root_cached { struct rb_root rb_root; struct rb_node *rb_leftmost; };
#define RB_ROOT (struct rb_root){ 0 }
#define RB_ROOT_CACHED (struct rb_root_cached){ {0}, 0 }
#define rb_entry(ptr,type,member) container_of(ptr,type,member)
#define rb_entry_safe(ptr,type,member) ({ __typeof__(ptr) ____p=(ptr); ____p?rb_entry(____p,type,member):(type*)0; })
#define RB_EMPTY_ROOT(root) ((root)->rb_node==0)
#define RB_EMPTY_NODE(node) ((node)->__rb_parent_color==(unsigned long)(node))
#define RB_CLEAR_NODE(node) ((node)->__rb_parent_color=(unsigned long)(node))
#define rb_parent(r) ((struct rb_node*)((r)->__rb_parent_color & ~3))
#ifdef __cplusplus
extern "C" {
#endif
void rb_insert_color(struct rb_node*, struct rb_root*);
void rb_erase(struct rb_node*, struct rb_root*);
struct rb_node *rb_next(const struct rb_node*);
struct rb_node *rb_prev(const struct rb_node*);
struct rb_node *rb_first(const struct rb_root*);
struct rb_node *rb_last(const struct rb_root*);
void rb_replace_node(struct rb_node*, struct rb_node*, struct rb_root*);
void rb_insert_color_cached(struct rb_node*, struct rb_root_cached*, bool);
void rb_erase_cached(struct rb_node*, struct rb_root_cached*);
struct rb_node *rb_first_cached(const struct rb_root_cached*);
#ifdef __cplusplus
}
#endif
static inline void rb_link_node(struct rb_node *node, struct rb_node *parent, struct rb_node **link){
  node->__rb_parent_color=(unsigned long)parent; node->rb_left=node->rb_right=0; *link=node; }
#define rbtree_postorder_for_each_entry_safe(pos,n,root,member) \
  for(pos=rb_entry_safe(rb_first(root),__typeof__(*pos),member); \
      pos&&((n=rb_entry_safe(rb_next(&pos->member),__typeof__(*pos),member)),1); pos=n)
#define rb_first_cached(root) ((root)->rb_leftmost)
#endif

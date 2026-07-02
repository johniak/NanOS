#ifndef _LKPI_INTERVAL_TREE_GENERIC_H
#define _LKPI_INTERVAL_TREE_GENERIC_H
#include <linux/rbtree.h>
#include <linux/rbtree_augmented.h>
/* Simplified interval tree: our own ITPREFIX##_insert does an ordered insert by START and the
 * iterators do a linear overlap scan (sufficient for small node sets). BUT the augment-callbacks
 * struct must be REAL: drm_mm.c does NOT use our _insert — its drm_mm_interval_tree_add_node
 * calls rb_insert_augmented_cached(&ITPREFIX##_augment) directly, which invokes augment->rotate
 * during rebalancing. A {0,0,0} struct there means a call through a NULL rotate pointer (#UD
 * storm / hang the moment the tree first rotates — i.e. the first real 3D GEM allocation).
 * Generate the proper max-subtree callbacks that maintain ITSUBTREE, exactly like upstream. */
/* NOTE: force `static` (not ITSTATIC) on the callbacks — callers pass `static inline`, and
 * `inline` on the const struct variable is meaningless (a warning). The struct is still emitted
 * because drm_mm references it via rb_insert_augmented_cached. */
#define INTERVAL_TREE_DEFINE(ITSTRUCT, ITRB, ITTYPE, ITSUBTREE, ITSTART, ITLAST, ITSTATIC, ITPREFIX) \
RB_DECLARE_CALLBACKS_MAX(static, ITPREFIX##_augment, ITSTRUCT, ITRB, ITTYPE, ITSUBTREE, ITLAST) \
ITSTATIC void ITPREFIX##_insert(ITSTRUCT *node, struct rb_root_cached *root){ \
  struct rb_node **p=&root->rb_root.rb_node, *parent=0; \
  while(*p){ parent=*p; if(ITSTART(node) < ITSTART(rb_entry(parent,ITSTRUCT,ITRB))) p=&(*p)->rb_left; else p=&(*p)->rb_right; } \
  rb_link_node(&node->ITRB, parent, p); rb_insert_color(&node->ITRB, &root->rb_root); } \
ITSTATIC void ITPREFIX##_remove(ITSTRUCT *node, struct rb_root_cached *root){ rb_erase(&node->ITRB, &root->rb_root); } \
ITSTATIC ITSTRUCT *ITPREFIX##_iter_first(struct rb_root_cached *root, ITTYPE start, ITTYPE last){ \
  struct rb_node *n=rb_first(&root->rb_root); \
  for(; n; n=rb_next(n)){ ITSTRUCT *e=rb_entry(n,ITSTRUCT,ITRB); if(ITSTART(e)<=last && ITLAST(e)>=start) return e; } return 0; } \
ITSTATIC ITSTRUCT *ITPREFIX##_iter_next(ITSTRUCT *node, ITTYPE start, ITTYPE last){ \
  struct rb_node *n=rb_next(&node->ITRB); \
  for(; n; n=rb_next(n)){ ITSTRUCT *e=rb_entry(n,ITSTRUCT,ITRB); if(ITSTART(e)<=last && ITLAST(e)>=start) return e; } return 0; }
#endif

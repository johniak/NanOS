#ifndef _LKPI_INTERVAL_TREE_GENERIC_H
#define _LKPI_INTERVAL_TREE_GENERIC_H
#include <linux/rbtree.h>
#include <linux/rbtree_augmented.h>
/* Simplified interval tree: ordered insert by START, linear iter for overlaps.
 * Also emits an (unused) augment-callbacks struct named ITPREFIX##_augment so
 * callers that reference it (drm_mm) link. Sufficient for small node sets. */
#define INTERVAL_TREE_DEFINE(ITSTRUCT, ITRB, ITTYPE, ITSUBTREE, ITSTART, ITLAST, ITSTATIC, ITPREFIX) \
ITSTATIC const struct rb_augment_callbacks ITPREFIX##_augment = { 0, 0, 0 }; \
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

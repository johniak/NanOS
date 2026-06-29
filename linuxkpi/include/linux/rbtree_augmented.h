#ifndef _LKPI_RBTREE_AUGMENTED_H
#define _LKPI_RBTREE_AUGMENTED_H
#include <linux/rbtree.h>
struct rb_augment_callbacks { void (*propagate)(struct rb_node*, struct rb_node*); void (*copy)(struct rb_node*, struct rb_node*); void (*rotate)(struct rb_node*, struct rb_node*); };
static inline void rb_insert_augmented(struct rb_node *node, struct rb_root *root, const struct rb_augment_callbacks *a){ (void)a; rb_insert_color(node, root); }
static inline void rb_insert_augmented_cached(struct rb_node *node, struct rb_root_cached *root, bool leftmost, const struct rb_augment_callbacks *a){ (void)leftmost;(void)a; rb_insert_color(node,&root->rb_root); }
static inline void rb_erase_augmented(struct rb_node *node, struct rb_root *root, const struct rb_augment_callbacks *a){ (void)a; rb_erase(node, root); }
static inline void rb_erase_augmented_cached(struct rb_node *node, struct rb_root_cached *root, const struct rb_augment_callbacks *a){ (void)a; rb_erase(node,&root->rb_root); }
/* RB_DECLARE_CALLBACKS_MAX: declare the augment-callbacks struct + a no-op compute (max-hole
 * tracking degrades to plain tree; drm_mm best-fit falls back to linear scan). */
#define RB_DECLARE_CALLBACKS_MAX(rbstatic, rbname, rbstruct, rbfield, rbtype, rbaugmented, rbcompute) \
  rbstatic const struct rb_augment_callbacks rbname = { 0, 0, 0 };
#define RB_DECLARE_CALLBACKS(rbstatic, rbname, ...) rbstatic const struct rb_augment_callbacks rbname = { 0, 0, 0 };
#endif

/* linuxkpi/include/linux/radix-tree.h — STUB. Only i915's userptr GEM (gem/i915_gem_userptr.c) uses
 * the radix tree, and userptr is NOT on the KMS/execbuf bring-up path. This provides the types so the
 * driver compiles; lookups return empty. FOLLOW-ON: vendor lib/radix-tree.c (or map to xarray) before
 * enabling userptr on the Dell. */
#ifndef _LINUXKPI_LINUX_RADIX_TREE_H
#define _LINUXKPI_LINUX_RADIX_TREE_H
#include <linux/types.h>
#include <linux/gfp.h>
struct radix_tree_root { int dummy; };
struct radix_tree_iter { unsigned long index; };
#define RADIX_TREE(name, mask) struct radix_tree_root name = { 0 }
#define INIT_RADIX_TREE(root, mask) do { (root)->dummy = 0; } while (0)
static inline int radix_tree_insert(struct radix_tree_root *r, unsigned long i, void *item)
{ (void)r;(void)i;(void)item; return 0; }
static inline void *radix_tree_lookup(const struct radix_tree_root *r, unsigned long i){ (void)r;(void)i; return 0; }
static inline void *radix_tree_delete(struct radix_tree_root *r, unsigned long i){ (void)r;(void)i; return 0; }
static inline bool radix_tree_empty(const struct radix_tree_root *r){ (void)r; return true; }
static inline void **radix_tree_iter_init(struct radix_tree_iter *it, unsigned long start){ it->index=start; return 0; }
static inline void **radix_tree_next_chunk(const struct radix_tree_root *r, struct radix_tree_iter *it, unsigned f){ (void)r;(void)it;(void)f; return 0; }
static inline void *radix_tree_deref_slot(void **slot){ return slot ? *slot : 0; }
static inline void radix_tree_iter_delete(struct radix_tree_root *r, struct radix_tree_iter *it, void **slot){ (void)r;(void)it;(void)slot; }
#define radix_tree_for_each_slot(slot, root, iter, start) \
	for (slot = radix_tree_iter_init(iter, start); slot; slot = radix_tree_next_chunk(root, iter, 0))
#endif

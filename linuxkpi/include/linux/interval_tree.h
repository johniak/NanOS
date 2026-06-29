#ifndef _LKPI_INTERVAL_TREE_H
#define _LKPI_INTERVAL_TREE_H
#include <linux/rbtree.h>
struct interval_tree_node { struct rb_node rb; unsigned long start, last; };
struct rb_root_cached;
void interval_tree_insert(struct interval_tree_node*, struct rb_root_cached*);
void interval_tree_remove(struct interval_tree_node*, struct rb_root_cached*);
struct interval_tree_node *interval_tree_iter_first(struct rb_root_cached*, unsigned long, unsigned long);
struct interval_tree_node *interval_tree_iter_next(struct interval_tree_node*, unsigned long, unsigned long);
#endif

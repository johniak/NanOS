/*
 * linuxkpi/include/linux/hashtable.h — self-contained small hashtable (i915 uses a few: engine
 * lookup, GuC context ids). Reproduces the upstream API over the shim's hlist ops without pulling
 * asm/hash.h or rculist.h. Keys are hashed with the 32-bit golden-ratio multiplicative hash — i915's
 * keys (ids, small handles) distribute fine; wide keys are truncated to 32 bits for the hash only
 * (still a valid distribution; buckets just share more).
 */
#ifndef _LINUXKPI_LINUX_HASHTABLE_H
#define _LINUXKPI_LINUX_HASHTABLE_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/kernel.h>   /* ARRAY_SIZE, ilog2 */

#define DECLARE_HASHTABLE(name, bits)  struct hlist_head name[1 << (bits)]
#define DEFINE_HASHTABLE(name, bits) \
	struct hlist_head name[1 << (bits)] = \
		{ [0 ... ((1 << (bits)) - 1)] = HLIST_HEAD_INIT }

#define HASH_SIZE(name)  (ARRAY_SIZE(name))
#define HASH_BITS(name)  ilog2(HASH_SIZE(name))

#define GOLDEN_RATIO_32  0x61C88647U

static inline u32 __lkpi_hash_min(unsigned long val, unsigned int bits)
{
	if (!bits) return 0;
	return ((u32) val * GOLDEN_RATIO_32) >> (32 - bits);
}
#define hash_min(val, bits)  __lkpi_hash_min((unsigned long)(val), (bits))

static inline void __hash_init(struct hlist_head *ht, unsigned int sz)
{
	unsigned int i;
	for (i = 0; i < sz; i++)
		INIT_HLIST_HEAD(&ht[i]);
}
#define hash_init(ht)  __hash_init(ht, HASH_SIZE(ht))

#define hash_add(ht, node, key) \
	hlist_add_head(node, &ht[hash_min(key, HASH_BITS(ht))])

static inline void hash_del(struct hlist_node *node)
{
	hlist_del_init(node);
}

static inline bool __hash_empty(struct hlist_head *ht, unsigned int sz)
{
	unsigned int i;
	for (i = 0; i < sz; i++)
		if (!hlist_empty(&ht[i]))
			return false;
	return true;
}
#define hash_empty(ht)  __hash_empty(ht, HASH_SIZE(ht))

#define hash_for_each(ht, bkt, obj, member) \
	for ((bkt) = 0; (bkt) < HASH_SIZE(ht); (bkt)++) \
		hlist_for_each_entry(obj, &ht[bkt], member)

#define hash_for_each_possible(ht, obj, member, key) \
	hlist_for_each_entry(obj, &ht[hash_min(key, HASH_BITS(ht))], member)

#endif /* _LINUXKPI_LINUX_HASHTABLE_H */

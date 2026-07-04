/*
 * linuxkpi/kpi_shrinker.c — shrinker registry (6.12 shrinker_alloc/register/free API).
 *
 * i915 registers a shrinker so the kernel can reclaim purgeable GEM pages under memory pressure.
 * NanOS does not yet drive reclaim from its allocator, so the registry is REAL — shrinkers are
 * allocated, linked into a list, and unlinked/freed correctly — but it is never invoked. A boot
 * notice states this. Wiring the kernel OOM path to walk this list and call ->scan_objects is a
 * recorded follow-on; i915 bring-up on an 8-16 GiB machine does not depend on reclaim.
 */
#include <linux/shrinker.h>
#include <linux/list.h>
#include <stdarg.h>
#include "lkpi_knx.h"

static struct list_head g_shrinkers = { &g_shrinkers, &g_shrinkers };
static int g_shrink_noticed;

struct shrinker *shrinker_alloc(unsigned int flags, const char *fmt, ...)
{
	struct shrinker *s = (struct shrinker *) knx_malloc(sizeof *s);
	char *z;
	unsigned i;
	(void) fmt;   /* the debug name is formatted from fmt+va in Linux; unused here */
	if (!s) return 0;
	z = (char *) s;
	for (i = 0; i < sizeof *s; i++) z[i] = 0;
	s->flags = flags;
	s->seeks = DEFAULT_SEEKS;
	s->batch = 0;
	INIT_LIST_HEAD(&s->list);
	return s;
}

void shrinker_register(struct shrinker *shrinker)
{
	if (!shrinker) return;
	list_add_tail(&shrinker->list, &g_shrinkers);
	if (!g_shrink_noticed) {
		knx_log("lkpi: shrinker registered (reclaim not wired)\n");
		g_shrink_noticed = 1;
	}
}

void shrinker_free(struct shrinker *shrinker)
{
	if (!shrinker) return;
	if (shrinker->list.next && shrinker->list.prev)
		list_del(&shrinker->list);
	knx_free(shrinker);
}

/* Test/introspection hook (not part of the Linux API): number of shrinkers currently registered. */
int lkpi_shrinker_registered_count(void)
{
	int n = 0;
	struct list_head *p;
	for (p = g_shrinkers.next; p != &g_shrinkers; p = p->next)
		n++;
	return n;
}

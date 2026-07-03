/*
 * linuxkpi/kpi_fence.c — dma_fence, dma_resv, dma_buf, dma_fence_chain for the shim.
 *
 * This is the LinuxKPI implementation of the cross-driver synchronization primitives the
 * unmodified DRM core and virtio_gpu driver use. Rather than lift drivers/dma-buf/*.c
 * (which would force the real dma_fence struct layout across all 57 already-compiled DRM
 * objects), we provide the API over the shim's own struct definitions — the FreeBSD
 * drm-kmod pattern. Semantics are faithful for the single-producer GPU command stream:
 *
 *   - dma_fence: real reference counting (kref), a callback list run on signal, and a
 *     signaled flag. Waiting spins cooperatively, pumping a registered poll hook so the
 *     virtqueue completion interrupt that signals the fence can make progress.
 *   - dma_resv: tracks the most-recent fence (GPU ops on one object are serialized), with
 *     ww_mutex locking. wait/test consult that fence.
 *   - dma_buf / dma_fence_chain: minimal — exercised only by the prime-export / timeline-
 *     syncobj paths, not by plain scanout.
 */
#include <linux/dma-fence.h>
#include <linux/dma-resv.h>
#include <linux/dma-buf.h>
#include <linux/dma-fence-chain.h>
#include <linux/ww_mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/errno.h>
#include <linux/jiffies.h>

/* The global ww_class every dma_resv shares (declared extern in <linux/ww_mutex.h>). */
struct ww_class reservation_ww_class = { 0 };

/* Optional cooperative-wait hook: the virtio transport sets this to pump the control
 * virtqueue so an in-flight fence can be signaled while another context waits on it. */
static void (*lkpi_fence_poll_hook)(void);
void lkpi_set_fence_poll(void (*fn)(void)) { lkpi_fence_poll_hook = fn; }

/* Called from __wait_event()'s spin (see <linux/wait.h>): pump the registered poll source
 * (virtio vq) so a cooperative boot-time wait on a vq ack/response makes progress. */
void lkpi_wait_pump(void) { if (lkpi_fence_poll_hook) lkpi_fence_poll_hook(); }

/* ---- dma_fence ---------------------------------------------------------------------- */

void dma_fence_init(struct dma_fence *f, const struct dma_fence_ops *ops,
		    spinlock_t *lock, u64 context, u64 seqno)
{
	f->ops = ops;
	f->lock = lock;
	f->context = context;
	f->seqno = seqno;
	f->flags = 0;
	f->error = 0;
	INIT_LIST_HEAD(&f->cb_list);
	kref_init(&f->refcount);
}

static void dma_fence_release(struct kref *kref)
{
	struct dma_fence *f = container_of(kref, struct dma_fence, refcount);
	if (f->ops && f->ops->release)
		f->ops->release(f);
	else
		kfree(f);
}

struct dma_fence *dma_fence_get(struct dma_fence *f)
{
	if (f)
		kref_get(&f->refcount);
	return f;
}

struct dma_fence *dma_fence_get_rcu(struct dma_fence *f) { return dma_fence_get(f); }

void dma_fence_put(struct dma_fence *f)
{
	if (f)
		kref_put(&f->refcount, dma_fence_release);
}

int dma_fence_signal_locked(struct dma_fence *f)
{
	struct dma_fence_cb *cb, *tmp;
	if (!f)
		return -EINVAL;
	if (test_and_set_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &f->flags))
		return -EINVAL;	/* already signaled */
	list_for_each_entry_safe(cb, tmp, &f->cb_list, node) {
		list_del_init(&cb->node);
		cb->func(f, cb);
	}
	return 0;
}

int dma_fence_signal(struct dma_fence *f)
{
	unsigned long flags;
	int ret;
	if (!f)
		return -EINVAL;
	if (f->lock)
		spin_lock_irqsave(f->lock, flags);
	ret = dma_fence_signal_locked(f);
	if (f->lock)
		spin_unlock_irqrestore(f->lock, flags);
	return ret;
}

bool dma_fence_is_signaled(struct dma_fence *f)
{
	if (!f)
		return true;
	if (test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &f->flags))
		return true;
	if (f->ops && f->ops->signaled && f->ops->signaled(f)) {
		dma_fence_signal(f);
		return true;
	}
	return false;
}

int dma_fence_add_callback(struct dma_fence *f, struct dma_fence_cb *cb,
			   dma_fence_func_t func)
{
	unsigned long flags;
	int ret = 0;
	if (!f || !cb || !func)
		return -EINVAL;
	if (f->lock)
		spin_lock_irqsave(f->lock, flags);
	if (test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &f->flags)) {
		ret = -ENOENT;
	} else {
		cb->func = func;
		list_add_tail(&cb->node, &f->cb_list);
		/* ask the implementation to start signaling if it is lazy about it */
		if (f->ops && f->ops->enable_signaling &&
		    !test_and_set_bit(DMA_FENCE_FLAG_ENABLE_SIGNAL_BIT, &f->flags)) {
			if (!f->ops->enable_signaling(f)) {
				/* enable_signaling says it is already done */
				list_del_init(&cb->node);
				ret = -ENOENT;
			}
		}
	}
	if (f->lock)
		spin_unlock_irqrestore(f->lock, flags);
	return ret;
}

long dma_fence_wait_timeout(struct dma_fence *f, bool intr, long timeout)
{
	unsigned long start;
	(void)intr;
	if (!f)
		return timeout;
	if (f->ops && f->ops->wait && f->ops->wait != (void *)0)
		return f->ops->wait(f, intr, timeout);
	/* The deadline is real time (jiffies, 1 ms each), not spin iterations: a pause+pump
	 * iteration is tens of nanoseconds, so counting iterations against a jiffies-denominated
	 * timeout would expire ~10^4 times too early and turn honest waits into spurious -EBUSY. */
	start = lkpi_jiffies();
	while (!dma_fence_is_signaled(f)) {
		if (lkpi_fence_poll_hook)
			lkpi_fence_poll_hook();
		__asm__ __volatile__("pause");
		if (timeout != MAX_SCHEDULE_TIMEOUT &&
		    (long)(lkpi_jiffies() - start) >= timeout)
			return 0;	/* timed out */
	}
	if (timeout == MAX_SCHEDULE_TIMEOUT)
		return timeout;
	{
		long left = timeout - (long)(lkpi_jiffies() - start);
		return left > 0 ? left : 1;
	}
}

long dma_fence_wait(struct dma_fence *f, bool intr)
{
	return dma_fence_wait_timeout(f, intr, MAX_SCHEDULE_TIMEOUT);
}

u64 dma_fence_context_alloc(unsigned num)
{
	static u64 ctr = 1;
	u64 first = ctr;
	ctr += num;
	return first;
}

/* a shared, permanently-signaled stub fence */
static const struct dma_fence_ops dma_fence_stub_ops = { 0 };
static struct dma_fence the_stub_fence;
static int stub_inited;

struct dma_fence *dma_fence_get_stub(void)
{
	if (!stub_inited) {
		static spinlock_t stub_lock;
		spin_lock_init(&stub_lock);
		dma_fence_init(&the_stub_fence, &dma_fence_stub_ops, &stub_lock, 0, 0);
		set_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &the_stub_fence.flags);
		stub_inited = 1;
	}
	return dma_fence_get(&the_stub_fence);
}

struct dma_fence *dma_fence_allocate_private_stub(ktime_t timestamp)
{
	struct dma_fence *f;
	static spinlock_t priv_lock;
	(void)timestamp;
	f = (struct dma_fence *)kzalloc(sizeof(*f), 0);
	if (!f)
		return 0;
	spin_lock_init(&priv_lock);
	dma_fence_init(f, &dma_fence_stub_ops, &priv_lock, dma_fence_context_alloc(1), 1);
	set_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &f->flags);
	return f;
}

/* ---- dma_resv ----------------------------------------------------------------------- */
/* Minimal: hold the single most-recent fence; GPU ops on one object are serialized. */

void dma_resv_init(struct dma_resv *r)
{
	ww_mutex_init(&r->lock, &reservation_ww_class);
	r->fences = 0;
}

void dma_resv_fini(struct dma_resv *r)
{
	if (r->fences) {
		dma_fence_put((struct dma_fence *)r->fences);
		r->fences = 0;
	}
}

int  dma_resv_lock(struct dma_resv *r, struct ww_acquire_ctx *ctx) { return ww_mutex_lock(&r->lock, ctx); }
int  dma_resv_lock_interruptible(struct dma_resv *r, struct ww_acquire_ctx *ctx) { return ww_mutex_lock_interruptible(&r->lock, ctx); }
bool dma_resv_trylock(struct dma_resv *r) { return ww_mutex_trylock(&r->lock, 0); }
void dma_resv_unlock(struct dma_resv *r) { ww_mutex_unlock(&r->lock); }

int dma_resv_reserve_fences(struct dma_resv *r, unsigned num) { (void)r; (void)num; return 0; }

unsigned long lkpi_resv_fence_adds;   /* NWDBG: proves execbuffer fences land on BO resvs */

void dma_resv_add_fence(struct dma_resv *r, struct dma_fence *f, enum dma_resv_usage usage)
{
	(void)usage;
	if (!r)
		return;
	lkpi_resv_fence_adds++;
	if (r->fences)
		dma_fence_put((struct dma_fence *)r->fences);
	r->fences = dma_fence_get(f);
}

long dma_resv_wait_timeout(struct dma_resv *r, enum dma_resv_usage usage, bool intr, long timeout)
{
	(void)usage;
	if (r && r->fences)
		return dma_fence_wait_timeout((struct dma_fence *)r->fences, intr, timeout);
	return timeout;
}

bool dma_resv_test_signaled(struct dma_resv *r, enum dma_resv_usage usage)
{
	/* NWDBG telemetry: prove the busy path is exercised (calls / no-fence / busy results). */
	static unsigned long nwdbg_calls, nwdbg_nofence, nwdbg_busy;
	bool sig;
	(void)usage;
	nwdbg_calls++;
	if (!r || !r->fences) {
		nwdbg_nofence++;
		if ((nwdbg_calls & 0x3ff) == 1)
			printk("lkpi resv_test: calls=%lu nofence=%lu busy=%lu\n",
			       nwdbg_calls, nwdbg_nofence, nwdbg_busy);
		return true;
	}
	/* NOWAIT busy-polls (DRM_IOCTL_VIRTGPU_WAIT) are how Mesa's virgl winsys decides a
	 * transfer-staging buffer may be recycled. We never wire the INTx line, so harvest the
	 * used ring once here — otherwise a completion that already arrived stays unobserved
	 * and every poll reports busy. The former always-true stub here let Mesa overwrite
	 * staging the host had not yet consumed: the GL desktop's cross-window texture shred. */
	if (lkpi_fence_poll_hook)
		lkpi_fence_poll_hook();
	sig = dma_fence_is_signaled((struct dma_fence *)r->fences);
	if (!sig)
		nwdbg_busy++;
	if ((nwdbg_calls & 0x3ff) == 1) {
		extern unsigned long lkpi_resv_fence_adds;
		printk("lkpi resv_test: calls=%lu nofence=%lu busy=%lu adds=%lu\n",
		       nwdbg_calls, nwdbg_nofence, nwdbg_busy, lkpi_resv_fence_adds);
	}
	return sig;
}

/* ---- dma_buf (prime export path; not on the scanout hot path) ----------------------- */

struct dma_buf *dma_buf_export(struct dma_buf_export_info *info)
{
	struct dma_buf *b;
	if (!info)
		return 0;
	b = (struct dma_buf *)kzalloc(sizeof(*b), 0);
	if (!b)
		return 0;
	b->size = info->size;
	b->ops = info->ops;
	b->priv = info->priv;
	b->resv = info->resv;
	b->exp_name = info->exp_name;
	return b;
}

void dma_buf_put(struct dma_buf *b)
{
	if (b) {
		if (b->ops && b->ops->release)
			b->ops->release(b);
		kfree(b);
	}
}

struct dma_buf *dma_buf_get(int fd) { (void)fd; return (struct dma_buf *)0 - 1 /* ERR_PTR(-EINVAL) sentinel */; }

struct dma_buf_attachment *dma_buf_attach(struct dma_buf *b, struct device *dev)
{
	struct dma_buf_attachment *a = (struct dma_buf_attachment *)kzalloc(sizeof(*a), 0);
	if (!a)
		return 0;
	a->dmabuf = b;
	a->dev = dev;
	return a;
}

void dma_buf_detach(struct dma_buf *b, struct dma_buf_attachment *a) { (void)b; kfree(a); }

/* ---- dma_fence_chain (timeline syncobj path; not on the scanout hot path) ----------- */

struct dma_fence_chain *to_dma_fence_chain(struct dma_fence *f)
{
	if (f && f->ops == 0)
		return 0;
	return container_of(f, struct dma_fence_chain, base);
}

int dma_fence_chain_find_seqno(struct dma_fence **pfence, uint64_t seqno)
{
	(void)pfence; (void)seqno;
	return 0;
}

struct dma_fence *dma_fence_chain_walk(struct dma_fence *fence)
{
	dma_fence_put(fence);
	return 0;
}

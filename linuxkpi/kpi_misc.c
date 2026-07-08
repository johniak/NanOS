/*
 * linuxkpi/kpi_misc.c — leaf kernel-infra the unmodified DRM/virtio stack links against.
 *
 *  - shmem page provider: gem_shmem backs each GEM object with a shmem "file"; drm_gem.c
 *    pulls one page per index via shmem_read_folio_gfp(). We model the file's mapping as a
 *    lazily-populated array of single-page folios (folio==page==kernel virtual address on
 *    the identity-mapped kernel), each from alloc_pages_exact(). This is the buffer memory
 *    the scanout DMAs from — the display hot path.
 *  - seq_file / sysfs / anon_inode / strtol / kasprintf: small helpers used by debug,
 *    sysfs show callbacks and the prime/syncobj fd paths.
 *  - global objects (boot_cpu_data, iomem_resource, the system_* workqueues, current):
 *    the data symbols the vendored code references.
 */
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/shmem_fs.h>
#include <linux/seq_file.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/ioport.h>
#include <linux/string.h>
#include <linux/printk.h>
#include <asm/cpufeature.h>
#include "lkpi_knx.h"    /* knx_uptime_us — spin-watchdog clock */

/* ---- global data objects ------------------------------------------------------------ */

/* .x86 = CPU family. TTM's ttm_prot_from_caching() gates PAT-based WC/UC mapping on
 * (family > 3); Comet Lake-U (and every 64-bit x86) is family 6. */
struct cpuinfo_x86 boot_cpu_data = { .x86_clflush_size = 64, .x86_cache_alignment = 64, .x86 = 6 };
struct resource iomem_resource = { .start = 0, .end = ~(resource_size_t)0, .name = "PCI mem" };

/* system_* workqueues are now defined by kpi_kthread.c (real async queues, Task 3). */

/* ---- stack-overflow tripwire (bring-up diagnostics) --------------------------------- *
 * The i915 probe runs synchronously on the loader's 1 MiB kernel stack (pre-scheduler). A runaway
 * recursion through an inline-run deferred primitive (unguarded queue_work, a dma_fence_signal
 * callback chain, an IRQ that re-enters submission) blows the stack and TRIPLE-FAULTS with no trace —
 * the CPU exception handler can itself fault from exception context (heavy console/FS path), so the
 * boot log ends mid-sentence with no rip. Capture the stack top once at probe entry; lkpi_stack_deep()
 * reports when we have descended past a redline, so a hot primitive can name the culprit's return
 * address via the RELIABLE FS-teed log — from NORMAL context, before the fault — and defer/return. */
static unsigned long g_lkpi_stack_top;
void lkpi_stack_baseline(void) { unsigned long sp; __asm__ __volatile__("mov %%rsp,%0" : "=r"(sp)); g_lkpi_stack_top = sp; }
int lkpi_stack_deep(void) {
	unsigned long sp;
	if (!g_lkpi_stack_top) return 0;
	__asm__ __volatile__("mov %%rsp,%0" : "=r"(sp));
	return (long)(g_lkpi_stack_top - sp) > (long)(640 * 1024);   /* 640 KiB of the 1 MiB stack consumed */
}
/* One global report (rip = the recursing call site), so a runaway loop leaves exactly one log line. */
void lkpi_deep_report(const char *where, void *ra) {
	static int once;
	if (once) return;
	once = 1;
	printk("lkpi: DEEP-STACK in %s ra=%p — deferring to break a runaway recursion\n", where, ra);
}

/* ---- spin watchdog (bring-up diagnostics) ------------------------------------------- *
 * The wait primitives now PUMP on every turn (service timers + workqueue + the virtio vq) so a wait
 * whose condition is satisfied by deferred work makes progress instead of starving. But a wait whose
 * condition can NEVER be satisfied still spins forever — silently, at the same code every boot. This
 * names it: each spin turn passes the caller's return address; once a single ra has been spinning for
 * more than 2 s, print it ONCE. A new call site restarts the clock, so healthy short waits never fire.
 * (Deliberately return-address-based rather than a separate watchdog thread: it works pre-scheduler,
 * needs no IRQ, and pins the exact starved wait for addr2line.) */
static void *g_spin_ra;
static unsigned long long g_spin_since_us;
static unsigned long long g_spin_last_us;
static int g_spin_reported;
void lkpi_spin_probe(void *ra) {
	unsigned long long now = knx_uptime_us();
	/* New site OR a >100 ms gap at the same site = a new wait episode, not the same spin —
	 * pump turns are back-to-back, so consecutive short waits from one callsite must not
	 * accumulate into a false report (see the raw/sleep probes below for the Dell evidence). */
	if (ra != g_spin_ra || (now - g_spin_last_us) > 100000ull) {
		g_spin_ra = ra;
		g_spin_since_us = now;
		g_spin_reported = 0;
		g_spin_last_us = now;
		return;
	}
	g_spin_last_us = now;
	if (!g_spin_reported && (now - g_spin_since_us) > 2000000ull) {
		unsigned long fl;
		int if_on;
		g_spin_reported = 1;
		/* Also report the interrupt-enable flag (RFLAGS.IF). This splits the two starvation classes at
		 * a glance: IF=0 means the wait runs with interrupts DISABLED (an outer spin_lock_irqsave CLI'd
		 * them), so a device MSI that would complete the awaited fence can never be delivered — the fix
		 * is to not hold irqsave across the wait, or to harvest the irq source in the pump. IF=1 means
		 * interrupts are live, so the awaited event genuinely never happens (e.g. the GPU never retired
		 * the first submission) — a real device-level problem, not a shim starvation. */
		__asm__ __volatile__("pushfq; popq %0" : "=r"(fl));
		if_on = (int)((fl >> 9) & 1);
		printk("lkpi: SPIN>2s ra=%p IF=%d spun=%llums — starved wait (IF=0: MSI blocked by CLI; IF=1: awaited event never occurs)\n",
		       ra, if_on, (now - g_spin_since_us) / 1000ull);
	}
}

/* Raw-busy-loop watchdog — separate state from lkpi_spin_probe. lkpi_spin_probe covers the PUMPED
 * waits (wait_event / dma_fence_wait / wait_for_completion); this one is called from cpu_relax() and
 * udelay(), the primitives a RAW i915 poll uses (`while (!(readl(reg) & BIT)) cpu_relax();` or a
 * timeout-less udelay poll). Such a loop never enters the pump, so lkpi_spin_probe never sees it and
 * no SPIN>2s fires — exactly the signature of the Dell freeze after "FIRST fence signal" (a bare GPU
 * register/HWSP poll that never completes because the GPU isn't advancing as i915 expects). Its own
 * g_cr_* state avoids thrashing g_spin_ra (cpu_relax alternating RAs with a pumped macro would reset
 * that deadline every iteration and silence the pumped watchdog). Logs the spinning site + RFLAGS.IF
 * once after 3s. */
static void *g_cr_ra;
static unsigned long long g_cr_since_us;
static unsigned long long g_cr_last_us;
static int g_cr_reported;
void lkpi_cpu_relax_probe(void *ra) {
	unsigned long long now = knx_uptime_us();
	/* Episode boundary: a genuine busy loop calls cpu_relax back-to-back (sub-µs gaps), so a
	 * >100 ms silence at the same ra means a NEW wait from the same callsite, not the same
	 * spin. Without this, consecutive short waits sharing one wait_for instantiation (GT
	 * workaround MMIO polls, AUX retries) accumulated across the whole probe and false-fired
	 * (Dell boots #40-44: RAW-SPIN at the same two ra's every boot, yet every wait completed). */
	if (ra != g_cr_ra || (now - g_cr_last_us) > 100000ull) {
		g_cr_ra = ra;
		g_cr_since_us = now;
		g_cr_reported = 0;
		g_cr_last_us = now;
		return;
	}
	g_cr_last_us = now;
	if (!g_cr_reported && (now - g_cr_since_us) > 3000000ull) {
		unsigned long fl;
		g_cr_reported = 1;
		__asm__ __volatile__("pushfq; popq %0" : "=r"(fl));
		printk("lkpi: RAW-SPIN>3s ra=%p IF=%d spun=%llums — timeout-less busy poll (cpu_relax/udelay); GPU register/HWSP never advanced\n",
		       ra, (int)((fl >> 9) & 1), (now - g_cr_since_us) / 1000ull);
	}
}

/* Sleep-poll watchdog — the THIRD class, separate state again. msleep / usleep_range / schedule_timeout
 * PUMP the deferred sources but (unlike the pumped waits above) were NOT probed, so an i915 poll written
 * `while (!cond) msleep(1);` / `... usleep_range() ...` / a schedule_timeout loop spins forever WITHOUT
 * tripping SPIN>2s or RAW-SPIN — exactly the Dell freeze-after-modeset signature (boot #26: probe never
 * returned, no watchdog fired, only the independent vblank IRQ flooded the log). Threshold 8 s, ABOVE the
 * longest legitimate single i915 sleep (panel power-cycle t11_t12 ~6 s, wait_for(..,5000) loops), so a
 * genuine legit wait never false-fires but an unbounded loop (runs tens of seconds) is named. Own g_sl_*
 * state so it never thrashes the 2 s / 3 s deadlines. */
static void *g_sl_ra;
static unsigned long long g_sl_since_us;
static unsigned long long g_sl_last_us;
static int g_sl_reported;
void lkpi_sleep_probe(void *ra) {
	unsigned long long now = knx_uptime_us();
	/* Episode boundary, same idea as the raw probe above but with a 1 s gap: a sleep-poll's
	 * iterations are at most the sleep length apart (i915 polls sleep 1-200 ms), so >1 s of
	 * silence at the same ra is a separate wait, not the same loop. The PPS panel-power waits
	 * all funnel through ONE wait_panel_status callsite and false-fired cumulatively. */
	if (ra != g_sl_ra || (now - g_sl_last_us) > 1000000ull) {
		g_sl_ra = ra;
		g_sl_since_us = now;
		g_sl_reported = 0;
		g_sl_last_us = now;
		return;
	}
	g_sl_last_us = now;
	if (!g_sl_reported && (now - g_sl_since_us) > 8000000ull) {
		unsigned long fl;
		g_sl_reported = 1;
		__asm__ __volatile__("pushfq; popq %0" : "=r"(fl));
		printk("lkpi: SLEEP-SPIN>8s ra=%p IF=%d spun=%llums — timeout-less poll via msleep/usleep_range/schedule_timeout; awaited condition never satisfied\n",
		       ra, (int)((fl >> 9) & 1), (now - g_sl_since_us) / 1000ull);
	}
}

/* The default comm names driver-internal contexts (probe, kthreads). DRM-node entry points stamp
 * the REAL client's pid+comm via lkpi_set_current_client below, so drm_ioctl/drm_release log
 * lines say which process (glkms/gles2info/nwm) issued the call — the old hard-coded
 * "virtio_gpu" labelled every i915 client on the Dell with the wrong driver's name. */
static struct task_struct lkpi_current_task = { .pid = 1, .comm = "lkpi", .mm = 0 };
struct task_struct *lkpi_current = &lkpi_current_task;

static char lkpi_client_comm[16];
void lkpi_set_current_client(int pid)
{
	lkpi_current_task.pid = pid;
	knx_process_comm(pid, lkpi_client_comm, sizeof(lkpi_client_comm));
	lkpi_current_task.comm = lkpi_client_comm;
}

/* ---- shmem page provider (gem_shmem backing store) ---------------------------------- */

static unsigned long lkpi_shmem_setups, lkpi_shmem_releases;

struct file *shmem_file_setup(const char *name, loff_t size, unsigned long flags)
{
	struct file *f;
	struct inode *ino;
	struct address_space *m;
	unsigned long npages = (unsigned long)((size + PAGE_SIZE - 1) / PAGE_SIZE);
	(void)name; (void)flags;
	lkpi_shmem_setups++;
	if ((lkpi_shmem_setups & 0x3f) == 0)
		printk("lkpi shmem: setups=%lu releases=%lu\n",
		       lkpi_shmem_setups, lkpi_shmem_releases);
	if (!npages)
		npages = 1;

	m = (struct address_space *)kzalloc(sizeof(*m), 0);
	ino = (struct inode *)kzalloc(sizeof(*ino), 0);
	f = (struct file *)kzalloc(sizeof(*f), 0);
	if (!m || !ino || !f) {
		kfree(m); kfree(ino); kfree(f);
		return 0;
	}
	m->pages = (struct page **)kzalloc(npages * sizeof(struct page *), 0);
	if (!m->pages) {
		kfree(m); kfree(ino); kfree(f);
		return 0;
	}
	/* Back the whole mapping with ONE physically-contiguous block and slice it into
	 * page-sized folios. Contiguity matters: a virtio-gpu scanout (and any user that
	 * wants a linear CPU framebuffer) needs page[0..n] adjacent so page_address(pages[0])
	 * is a flat buffer — the shim has no vmap that stitches scattered pages together.
	 * (RAM is identity-mapped, so contiguous virtual == contiguous physical.) */
	{
		unsigned char *blk = (unsigned char *)alloc_pages_exact(npages * PAGE_SIZE, 0);
		unsigned long i;
		if (!blk) {
			kfree(m->pages); kfree(m); kfree(ino); kfree(f);
			return 0;
		}
		memset(blk, 0, npages * PAGE_SIZE);
		/* store the mem_map ENTRY for each frame (not the data address): a struct page* is now
		 * a mem_map index, and page_to_phys()/page_address() derive the data from it. blk is
		 * contiguous+page-aligned, so these are consecutive mem_map entries. */
		for (i = 0; i < npages; i++) {
			m->pages[i] = virt_to_page(blk + i * PAGE_SIZE);
			set_page_count(m->pages[i], 1);   /* sane refcount for page_count() readers */
		}
	}
	m->nrpages = npages;
	m->host = ino;
	/* i_size must reflect the backing: shmem_pin_map (gt/shmem_utils.c) computes
	 * n_pages = file->f_mapping->host->i_size >> PAGE_SHIFT, so a zero i_size yields a
	 * zero-page vmap and a silently-empty default_state. Set the REQUESTED size (rounded to the
	 * page-granular backing we actually allocated). */
	ino->i_size = (loff_t)npages * PAGE_SIZE;
	ino->i_mapping = m;
	f->f_mapping = m;
	f->f_inode = ino;
	/* Born with ONE reference (mainline shmem_file_setup semantics). i915 takes a SECOND via
	 * shmem_create_from_object() -> atomic_long_inc(&f->f_count) to keep engine->default_state alive
	 * past drm_gem_object_release()'s fput(). fput() below now honours this count, so the shared
	 * backing survives until BOTH the object and default_state are released. */
	atomic_long_set(&f->f_count, 1);
	return f;
}

/* Fetch pages[index] with validation. Real Linux shmem_read_mapping_page never returns NULL — always
 * a valid page or an ERR_PTR — and its callers (i915 gt/shmem_utils.c __shmem_rw) only guard with
 * IS_ERR(), which rejects [-4095,-1] but NOT NULL and NOT a corrupt pointer. So a NULL (index past the
 * eagerly-populated backing) or a garbage entry would sail through into kmap()/page_to_virt() and #PF
 * on the copy. Seen on the Dell: lrc_alloc reading ce->default_state (intel_lrc.c:1021) with a length
 * past the file → page_to_virt(bad) → cr2 in the 0xffffff.. range. Validate index-in-range AND that
 * the entry is a real mem_map page; on failure log-once (which case + caller return address) and hand
 * back ERR_PTR(-EFAULT) so the driver's error path unwinds cleanly instead of faulting. The log names
 * the root cause on the next boot: OUT-OF-RANGE = a size mismatch (backing smaller than the read);
 * BAD page = an entry never set to a virt_to_page() mem_map index. */
static struct page *lkpi_shmem_page_at(struct address_space *mapping, unsigned long index, void *ra)
{
	struct page *p;
	if (!mapping || !mapping->pages || index >= mapping->nrpages) {
		static int once;
		if (!once) { once = 1; printk("lkpi shmem: read OUT-OF-RANGE index=%lu nrpages=%lu map=%p ra=%p\n",
			index, mapping ? mapping->nrpages : 0UL, (void *)mapping, ra); }
		return (struct page *)(long)-EFAULT;
	}
	p = mapping->pages[index];
	if (!p || (unsigned long)p < (unsigned long)lkpi_mem_map ||
	    page_to_pfn(p) >= lkpi_mem_map_pfns) {
		static int once;
		if (!once) { once = 1; printk("lkpi shmem: read BAD page[%lu]=%p nrpages=%lu ra=%p\n",
			index, (void *)p, mapping->nrpages, ra); }
		return (struct page *)(long)-EFAULT;
	}
	return p;
}

/* Pages are pre-populated (one contiguous block, see shmem_file_setup); hand back a validated `index`. */
struct folio *shmem_read_folio_gfp(struct address_space *mapping, unsigned long index, unsigned gfp)
{
	(void)gfp;
	return (struct folio *)lkpi_shmem_page_at(mapping, index, __builtin_return_address(0));
}

/* Same backing as shmem_read_folio_gfp, but returns the page (i915 gt/shmem_utils path). */
struct page *shmem_read_mapping_page_gfp(struct address_space *mapping, unsigned long index, unsigned gfp)
{
	(void)gfp;
	return lkpi_shmem_page_at(mapping, index, __builtin_return_address(0));
}

/* drm_gem unwind / shmem teardown: free the single contiguous backing block (= pages[0]). */
void lkpi_shmem_release(struct file *f)
{
	struct address_space *m;
	if (!f || !f->f_mapping)
		return;
	lkpi_shmem_releases++;
	/* Mirror the setup-side stat line: boot #40 printed setups=64 releases=0 MID-test (the
	 * counter fires every 64 setups, before drm_file close + the async i915 free worker ran)
	 * — a release-side line distinguishes "frees are just late" from a real leak. */
	if ((lkpi_shmem_releases & 0x3f) == 0)
		printk("lkpi shmem: setups=%lu releases=%lu\n",
		       lkpi_shmem_setups, lkpi_shmem_releases);
	m = f->f_mapping;
	if (m->pages) {
		if (m->nrpages && m->pages[0])
			free_pages_exact(page_address(m->pages[0]), m->nrpages * PAGE_SIZE);
		kfree(m->pages);
	}
	kfree(m->host);
	kfree(m);
	kfree(f);
}

/* Drop ONE reference to a backing file; free only when the last one goes.
 *
 * A file is born with f_count=1 (shmem_file_setup / anon_inode_getfile). i915 can take a SECOND
 * ref — shmem_create_from_object() does atomic_long_inc(&f->f_count) so engine->default_state
 * keeps the object's shmem backing alive after drm_gem_object_release() fput()s it. The old fput()
 * freed unconditionally, so that first fput() destroyed the file while default_state still pointed
 * at it: a use-after-free whose freed+zeroed f_mapping surfaced as `lkpi shmem: read OUT-OF-RANGE
 * map=0x0` when lrc_init_state() later read the golden context — and, because lrc_init_state()
 * ignores that read's result and still marks the context VALID + restore, the GPU restored an
 * uninitialised context image (an engine-wedge / probe-hang cascade). Honour the count instead.
 *
 * The virtio_gpu driver defers its fput to the RESOURCE_UNREF response callback, and the ctrl queue
 * is processed in order, so the host has consumed any pending transfer before the count hits zero.
 * Files without a mapping (sync/anon stubs) are skipped by lkpi_shmem_release harmlessly. */
void fput(struct file *f)
{
	if (!f)
		return;
	if (atomic_long_dec_and_test(&f->f_count))
		lkpi_shmem_release(f);
}

/* ---- seq_file (debug/sysfs output sink) --------------------------------------------- */

int seq_printf(struct seq_file *m, const char *fmt, ...) { (void)m; (void)fmt; return 0; }
void seq_puts(struct seq_file *m, const char *s) { (void)m; (void)s; }
void seq_putc(struct seq_file *m, char c) { (void)m; (void)c; }
/* No real file read pipeline in the shim: report immediate EOF (0 bytes). */
long seq_read(struct file *f, char __user *buf, unsigned long size, loff_t *ppos)
{ (void)f; (void)buf; (void)size; (void)ppos; return 0; }

/* wait_queue wake callbacks. wake_up() is a barrier in the shim (waiters spin on the condition), so
 * these are invoked only if a caller walks the queue itself; autoremove unlinks the entry as Linux does. */
int default_wake_function(struct wait_queue_entry *e, unsigned mode, int sync, void *key)
{ (void)e; (void)mode; (void)sync; (void)key; return 1; }
int autoremove_wake_function(struct wait_queue_entry *e, unsigned mode, int sync, void *key)
{ (void)mode; (void)sync; (void)key; if (e) list_del_init(&e->entry); return 1; }

/* kobject_create_and_add: heap-allocate a bare kobject (no sysfs tree in the shim). */
struct kobject *kobject_create_and_add(const char *name, struct kobject *parent)
{
	struct kobject *k = (struct kobject *)kzalloc(sizeof(*k), 0);
	if (k) { k->name = name; k->parent = parent; }
	return k;
}
void kobject_put(struct kobject *k) { kfree(k); }

/* one shared var-event waitqueue: wake_up is a barrier and waiters re-check their own condition,
 * so a single global queue is correct (see wait_bit.h). */
static struct wait_queue_head __lkpi_var_wq;
struct wait_queue_head *__var_waitqueue(void *p) { (void)p; return &__lkpi_var_wq; }

/* ---- sysfs string helpers ----------------------------------------------------------- */

int sysfs_streq(const char *a, const char *b)
{
	if (!a || !b)
		return 0;
	while (*a && *b && *a == *b) { a++; b++; }
	if ((*a == 0 || (*a == '\n' && a[1] == 0)) &&
	    (*b == 0 || (*b == '\n' && b[1] == 0)))
		return 1;
	return 0;
}

long sysfs_emit(char *buf, const char *fmt, ...)
{
	va_list ap; int n;
	if (!buf)
		return 0;
	va_start(ap, fmt);
	n = vsnprintf(buf, PAGE_SIZE, fmt, ap);
	va_end(ap);
	return n;
}

long sysfs_emit_at(char *buf, int at, const char *fmt, ...)
{
	va_list ap; int n;
	if (!buf)
		return 0;
	va_start(ap, fmt);
	n = vsnprintf(buf + at, PAGE_SIZE - at, fmt, ap);
	va_end(ap);
	return n;
}

/* CLASS_ATTR_STRING show: print the attribute's bound string. */
struct class; struct class_attribute;
struct class_attribute_string;
long show_class_attr_string(const struct class *c, const struct class_attribute *a, char *buf)
{
	/* layout: struct class_attribute_string { struct class_attribute attr; char *str; } */
	const char *str = *(const char * const *)((const char *)a + sizeof(struct class_attribute));
	(void)c;
	return sysfs_emit(buf, "%s\n", str ? str : "");
}

/* ---- string / format helpers -------------------------------------------------------- */

long simple_strtol(const char *cp, char **endp, unsigned int base)
{
	long v = 0; int neg = 0;
	if (!cp) { if (endp) *endp = (char *)cp; return 0; }
	while (*cp == ' ' || *cp == '\t') cp++;
	if (*cp == '-') { neg = 1; cp++; } else if (*cp == '+') cp++;
	if (base == 0) {
		if (cp[0] == '0' && (cp[1] == 'x' || cp[1] == 'X')) { base = 16; cp += 2; }
		else if (cp[0] == '0') { base = 8; cp++; }
		else base = 10;
	} else if (base == 16 && cp[0] == '0' && (cp[1] == 'x' || cp[1] == 'X')) {
		cp += 2;
	}
	for (;;) {
		int d;
		char ch = *cp;
		if (ch >= '0' && ch <= '9') d = ch - '0';
		else if (ch >= 'a' && ch <= 'z') d = ch - 'a' + 10;
		else if (ch >= 'A' && ch <= 'Z') d = ch - 'A' + 10;
		else break;
		if ((unsigned)d >= base) break;
		v = v * base + d;
		cp++;
	}
	if (endp) *endp = (char *)cp;
	return neg ? -v : v;
}

char *kvasprintf(gfp_t gfp, const char *fmt, va_list ap)
{
	va_list aq;
	int len;
	char *buf;
	__builtin_va_copy(aq, ap);
	len = vsnprintf(0, 0, fmt, aq);
	va_end(aq);
	if (len < 0)
		return 0;
	buf = (char *)kmalloc((size_t)len + 1, gfp);
	if (!buf)
		return 0;
	vsnprintf(buf, (size_t)len + 1, fmt, ap);
	return buf;
}

char *kasprintf(gfp_t gfp, const char *fmt, ...)
{
	va_list ap;
	char *r;
	va_start(ap, fmt);
	r = kvasprintf(gfp, fmt, ap);
	va_end(ap);
	return r;
}

/* ---- anon_inode / llseek (prime/syncobj fd path) ------------------------------------ */

struct file *anon_inode_getfile(const char *name, const struct file_operations *ops,
				void *priv, int flags)
{
	struct file *f;
	(void)name; (void)flags;
	f = (struct file *)kzalloc(sizeof(*f), 0);
	if (!f)
		return 0;
	f->f_op = ops;
	f->private_data = priv;
	atomic_long_set(&f->f_count, 1);   /* one reference; fput() frees at zero (see fput) */
	return f;
}

/* anon_inode_getfd: install a file over an anonymous inode and return its fd. The shim has no
 * process fd table at kext scope, so the backing file is created (identical to
 * anon_inode_getfile) but no descriptor is installed — callers in the DRM prime/syncobj export
 * path that only need the struct file* use anon_inode_getfile directly; the few that take the
 * fd get a stub descriptor. Full fd-table integration is part of the DRM render-node lift. */
int anon_inode_getfd(const char *name, const struct file_operations *ops, void *priv, int flags)
{
	struct file *f = anon_inode_getfile(name, ops, priv, flags);
	if (!f)
		return -ENOMEM;
	return -ENOSYS;   /* no kext-scope fd table; see comment above */
}

/* x86 TSC frequency in kHz. i915's GT PMU / timestamp code reads it; a real per-boot value is
 * supplied by the kernel via kexports on hardware bring-up. Non-zero default avoids div-by-zero. */
unsigned int tsc_khz = 1000000;   /* 1 GHz placeholder until knx supplies the calibrated value */

/* task_pid: the shim has no separate struct pid; identity is the task_struct pointer itself.
 * i915 stores/compares the returned token (get_pid/put_pid are no-ops), so an opaque cast
 * preserves the only property the driver relies on — pointer identity per task. */
struct pid *task_pid(struct task_struct *t) { return (struct pid *)t; }

loff_t noop_llseek(struct file *file, loff_t offset, int whence)
{
	(void)whence;
	if (file)
		file->f_pos = offset;
	return offset;
}

/* ---- rbtree cached variants (not in 6.12 lib/rbtree.c) ------------------------------ */
#include <linux/rbtree.h>

void rb_insert_color_cached(struct rb_node *node, struct rb_root_cached *root, bool leftmost)
{
	if (leftmost)
		root->rb_leftmost = node;
	rb_insert_color(node, &root->rb_root);
}

void rb_erase_cached(struct rb_node *node, struct rb_root_cached *root)
{
	if (root->rb_leftmost == node)
		root->rb_leftmost = rb_next(node);
	rb_erase(node, &root->rb_root);
}

/* ---- virtio_dma_buf (prime export) -------------------------------------------------- */
#include <linux/dma-buf.h>
struct dma_buf *virtio_dma_buf_export(struct dma_buf_export_info *info)
{
	return dma_buf_export(info);
}

/* ---- HDMI infoframes ----------------------------------------------------------------- *
 * WEAK stubs: the virtio_gpu link has no real HDMI sink and pulls only drm_edid (which calls
 * hdmi_avi_infoframe_init), so these satisfy it. The i915 link DOES include the real
 * drivers/video/hdmi.c, whose strong definitions override these — no multiple-definition clash. */
#include <linux/hdmi.h>
__attribute__((weak)) void hdmi_avi_infoframe_init(struct hdmi_avi_infoframe *frame)
{
	if (frame) { memset(frame, 0, sizeof(*frame)); frame->type = HDMI_INFOFRAME_TYPE_AVI; frame->version = 2; frame->length = 13; }
}
__attribute__((weak)) int hdmi_vendor_infoframe_init(struct hdmi_vendor_infoframe *frame)
{
	if (frame) { memset(frame, 0, sizeof(*frame)); frame->type = HDMI_INFOFRAME_TYPE_VENDOR; frame->version = 1; }
	return 0;
}

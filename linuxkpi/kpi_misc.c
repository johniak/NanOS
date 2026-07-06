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

static struct task_struct lkpi_current_task = { .pid = 1, .comm = "virtio_gpu", .mm = 0 };
struct task_struct *lkpi_current = &lkpi_current_task;

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
	ino->i_mapping = m;
	f->f_mapping = m;
	f->f_inode = ino;
	return f;
}

/* Pages are pre-populated (one contiguous block, see shmem_file_setup); just hand back `index`. */
struct folio *shmem_read_folio_gfp(struct address_space *mapping, unsigned long index, unsigned gfp)
{
	(void)gfp;
	if (!mapping || !mapping->pages || index >= mapping->nrpages)
		return 0;
	return (struct folio *)mapping->pages[index];
}

/* Same backing as shmem_read_folio_gfp, but returns the page (i915 gt/shmem_utils path). */
struct page *shmem_read_mapping_page_gfp(struct address_space *mapping, unsigned long index, unsigned gfp)
{
	(void)gfp;
	if (!mapping || !mapping->pages || index >= mapping->nrpages)
		return 0;
	return mapping->pages[index];
}

/* drm_gem unwind / shmem teardown: free the single contiguous backing block (= pages[0]). */
void lkpi_shmem_release(struct file *f)
{
	struct address_space *m;
	if (!f || !f->f_mapping)
		return;
	lkpi_shmem_releases++;
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

/* The last reference to a GEM object's backing file is dropped by drm_gem_object_release()
 * via fput() — this is where the pages actually die. The virtio_gpu driver defers this to
 * the RESOURCE_UNREF response callback, and the ctrl queue is processed in order, so the
 * host has always consumed any pending transfer from these pages by the time we free them.
 * Files without a mapping (sync/anon stubs) fall through harmlessly inside the release. */
void fput(struct file *f)
{
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

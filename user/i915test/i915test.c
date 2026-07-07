/*
 * i915test.nxe — the GEM/execbuf oracle (Plan 3 Task 8, mirrors igt's gem_exec_store).
 *
 * Default run — proves the whole GPU-compute path of the unmodified i915 with raw ioctls
 * (no libdrm, vendored uapi headers — byte-identical structs/ioctl numbers to the driver):
 *   GEM_CREATE two BOs (batch + target, shmem system pages)
 *   GEM_MMAP_OFFSET + mmap the batch (the i915_drm_node.c mmap-offset resolution)
 *   write a gen8+ MI_STORE_DWORD_IMM batch storing a magic dword into the target
 *   EXECBUFFER2 with both BOs SOFTPINNED (EXEC_OBJECT_PINNED — the same relocation-free
 *     ABI Mesa iris uses) on the default per-file context, render engine
 *   GEM_WAIT on the target (implicit exclusive fence from EXEC_OBJECT_WRITE)
 *   mmap the target and assert the dword — GPU-written memory read back by the CPU.
 * Success marker: "i915test: store OK" (first seen on the Dell: boot #33, 2026-07-07).
 *
 * `i915test hang` — the deliberate-hang/engine-reset validation (plan Task 8's one-time
 * test; requires hangcheck enabled in the glue): submits a spin-forever batch
 * (MI_BATCH_BUFFER_START jumping to its own start), waits for the heartbeat to declare a
 * hang and reset the engine (GEM_WAIT completes instead of timing out), then runs the
 * normal store test to prove the GPU came back alive. Markers:
 *   "i915test: hang recovered (engine reset)" + "i915test: store OK".
 *
 * Boots #34/#35 falsified the naive checks: GEM_WAIT completing does NOT mean the batch
 * executed (a skipped/cancelled request signals its fence too, and GEM_WAIT never
 * surfaces fence errors) — and #35 showed a VIRGIN context whose spin "completed" in
 * 1 ms with guilty=0: breadcrumbs ran, batches never did, no hang was ever declared.
 * So the hang path maps the whole space in one boot:
 *   - phase 0: store BEFORE any hang (broken-from-birth vs broken-by-reset);
 *   - phase 1: the spin (elapsed ms tells spun-and-reset [~seconds] from never-spun
 *     [~1 ms]); phase 2: store after; phase 3: store on a FRESH fd (per-context vs
 *     engine-wide damage);
 *   - every submit does a zero-timeout GEM_WAIT poll right after EXECBUFFER2
 *     ("BORN-COMPLETE" = stale timeline seqno, nothing ever executed) and a GEM_BUSY
 *     query after a successful wait ("wait lied" detector);
 *   - a failed readback re-reads after 2 s (late MAGIC = wait raced real execution);
 *   - RESET_STATS (guilty_count of the default context) printed after every phase.
 *
 * On a store-submit failure the test walks a DISCRIMINATION LADDER of minimal submits
 * (added after boot #32, where the errno was swallowed and cost a boot):
 *   B: MI_BATCH_BUFFER_END-only batch, single BO, kernel-placed (no softpin, no NO_RELOC)
 *      -> exercises context + engine + request + vm-bind alone.
 *   C: the same NOP batch, softpinned + NO_RELOC -> isolates EXEC_OBJECT_PINNED handling.
 * Every failure prints errno (ioctl() is -1/errno per reterr in libc-glue/syscalls.c).
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdint.h>
#include <drm/drm.h>
#include <drm/i915_drm.h>

/* Every marker is teed to the on-stick log (append, flushed per line) so
 * `make i915-log` pulls the verdict off the stick — no console photos needed. The
 * userland VFS has the root disk at /disks/main (there is NO /nanos alias — a bare
 * "/nanos/..." fopen cost boot #35 its tee with ENOENT); debugfs-side the same file
 * is /nanos/logs/i915test.txt. The macro reroutes all printf call sites below; the
 * tee is best-effort (NULL = console only). */
#define TEE_PATH "/disks/main/nanos/logs/i915test.txt"
static FILE *g_tee;

static int tee_printf(const char *fmt, ...)
{
	va_list ap;
	int r;
	va_start(ap, fmt);
	r = vprintf(fmt, ap);
	va_end(ap);
	if (g_tee) {
		va_start(ap, fmt);
		vfprintf(g_tee, fmt, ap);
		va_end(ap);
		fflush(g_tee);
	}
	return r;
}
#define printf tee_printf

#define MAGIC      0xC0DE1915u
/* Softpin GPU VAs: low, page-aligned, < 4 GiB (no EXEC_OBJECT_SUPPORTS_48B_ADDRESS
 * needed). Each store phase gets its own base (batch @base, target @base+0x80000):
 * pre-hang 0x100000, spin 0x200000, post-hang 0x300000, fresh-fd 0x100000 (new vm). */
#define SPIN_VA    0x200000ull      /* the hang test's self-looping batch                 */
#define STORE_OFF  64ull            /* store lands at target VA + 64                      */
#define NOP_OFF    2048u            /* MI_BB_END-only batch at +2048 in the batch BO      */

static int die(const char *step)
{
	printf("i915test: FAIL %s (errno=%d)\n", step, errno);
	return 1;
}

static long long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* guilty_count of the default context (RESET_STATS batch_active): the hang-attribution
 * ledger. guilty going UP across a phase means hangcheck shot THAT phase's batch. */
static void reset_stats(int fd, const char *when)
{
	struct drm_i915_reset_stats rs;
	memset(&rs, 0, sizeof rs);
	rs.ctx_id = 0;
	if (ioctl(fd, DRM_IOCTL_I915_GET_RESET_STATS, &rs))
		printf("i915test: RESET_STATS %s: errno=%d\n", when, errno);
	else
		printf("i915test: RESET_STATS %s: guilty=%u pending=%u resets=%u\n",
		       when, rs.batch_active, rs.batch_pending, rs.reset_count);
}

static uint32_t gem_create(int fd, uint64_t size)
{
	struct drm_i915_gem_create c;
	memset(&c, 0, sizeof c);
	c.size = size;
	if (ioctl(fd, DRM_IOCTL_I915_GEM_CREATE, &c))
		return 0;
	return c.handle;
}

static void *gem_mmap_wc(int fd, uint32_t handle, uint64_t size, const char *what)
{
	struct drm_i915_gem_mmap_offset mo;
	void *p;
	memset(&mo, 0, sizeof mo);
	mo.handle = handle;
	mo.flags  = I915_MMAP_OFFSET_WC;
	if (ioctl(fd, DRM_IOCTL_I915_GEM_MMAP_OFFSET, &mo)) {
		printf("i915test: MMAP_OFFSET(%s) failed (errno=%d)\n", what, errno);
		return 0;
	}
	p = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mo.offset);
	if (p == MAP_FAILED) {
		printf("i915test: mmap(%s @0x%llx) failed (errno=%d)\n",
		       what, (unsigned long long)mo.offset, errno);
		return 0;
	}
	return p;
}

static unsigned gem_busy(int fd, uint32_t handle)
{
	struct drm_i915_gem_busy b;
	memset(&b, 0, sizeof b);
	b.handle = handle;
	if (ioctl(fd, DRM_IOCTL_I915_GEM_BUSY, &b))
		return 0xdead;
	return b.busy;
}

/* One EXECBUFFER2 + GEM_WAIT(wait_handle), n objects (batch last). Returns 0 or -errno. */
static int submit(int fd, struct drm_i915_gem_exec_object2 *obj, int n,
		  uint64_t ebflags, uint32_t start, uint32_t wait_handle,
		  long long wait_ns, const char *label)
{
	struct drm_i915_gem_execbuffer2 eb;
	memset(&eb, 0, sizeof eb);
	eb.buffers_ptr        = (uintptr_t)obj;
	eb.buffer_count       = n;
	eb.batch_start_offset = start;
	eb.batch_len          = 0;              /* 0 = to the end of the batch BO */
	eb.flags              = ebflags;
	eb.rsvd1              = 0;              /* default per-file context */
	if (ioctl(fd, DRM_IOCTL_I915_GEM_EXECBUFFER2, &eb)) {
		printf("i915test: %s: EXECBUFFER2 errno=%d\n", label, errno);
		return -errno;
	}
	{
		/* Zero-timeout poll straight after submit: a request that is ALREADY
		 * complete here was born signalled (stale timeline seqno) — the GPU never
		 * executed anything. Boot #35 discriminator. */
		struct drm_i915_gem_wait w0;
		memset(&w0, 0, sizeof w0);
		w0.bo_handle  = wait_handle;
		w0.timeout_ns = 0;
		if (!ioctl(fd, DRM_IOCTL_I915_GEM_WAIT, &w0))
			printf("i915test: %s: BORN-COMPLETE (done 0 ms after submit)\n",
			       label);
	}
	{
		struct drm_i915_gem_wait w;
		long long t0 = now_ms();
		unsigned busy;
		memset(&w, 0, sizeof w);
		w.bo_handle  = wait_handle;
		w.timeout_ns = wait_ns;             /* bounded — never open-ended */
		if (ioctl(fd, DRM_IOCTL_I915_GEM_WAIT, &w)) {
			printf("i915test: %s: GEM_WAIT errno=%d after %lld ms\n",
			       label, errno, now_ms() - t0);
			return -errno;
		}
		/* Elapsed matters: a healthy store completes in ~0 ms; several SECONDS
		 * means the batch hung and hangcheck force-signalled it (no execution). */
		printf("i915test: %s: OK (wait %lld ms)\n", label, now_ms() - t0);
		busy = gem_busy(fd, wait_handle);
		if (busy)
			printf("i915test: %s: GEM_BUSY=0x%x AFTER a successful wait -- the wait lied\n",
			       label, busy);
	}
	return 0;
}

/* The gem_exec_store oracle at caller-chosen GPU VAs (batch @base, target @base+0x80000
 * — distinct per phase so no run rebinds another's addresses). Returns 0 on a verified
 * store; on a failed readback re-reads after 2 s (a late-landing MAGIC = the wait lied /
 * raced, still-zero = the batch never executed); on a failed submit runs the ladder. */
static int run_store(int fd, uint64_t base, const char *tag)
{
	uint64_t batch_va  = base;
	uint64_t target_va = base + 0x80000ull;
	uint32_t target = gem_create(fd, 4096);
	uint32_t batch  = gem_create(fd, 4096);
	uint32_t *bb;
	volatile uint32_t *tp;
	struct drm_i915_gem_exec_object2 obj[2];
	char lbl[64];
	int ra;
	if (!target || !batch)
		return die("GEM_CREATE");
	printf("i915test: %s: BOs created (target %u, batch %u)\n", tag, target, batch);

	bb = (uint32_t *)gem_mmap_wc(fd, batch, 4096, "batch");
	if (!bb)
		return die("mmap batch");
	/* gen8+ MI_STORE_DWORD_IMM: (0x20<<23)|2, addr lo/hi (ppgtt, bit22=0), data. */
	bb[0] = (0x20u << 23) | 2;
	bb[1] = (uint32_t)(target_va + STORE_OFF);
	bb[2] = (uint32_t)((target_va + STORE_OFF) >> 32);
	bb[3] = MAGIC;
	bb[4] = 0x0A << 23;                       /* MI_BATCH_BUFFER_END */
	bb[NOP_OFF / 4] = 0x0A << 23;             /* rung B/C: NOP-only batch */
	printf("i915test: %s: batch written (store 0x%08x -> GPU VA 0x%llx)\n",
	       tag, MAGIC, (unsigned long long)(target_va + STORE_OFF));

	/* Rung A — the real thing: two BOs, softpinned, store + verify. */
	memset(obj, 0, sizeof obj);
	obj[0].handle = target;                   /* batch LAST (no I915_EXEC_BATCH_FIRST) */
	obj[0].offset = target_va;
	obj[0].flags  = EXEC_OBJECT_PINNED | EXEC_OBJECT_WRITE;
	obj[1].handle = batch;
	obj[1].offset = batch_va;
	obj[1].flags  = EXEC_OBJECT_PINNED;
	snprintf(lbl, sizeof lbl, "%s softpin", tag);
	ra = submit(fd, obj, 2, I915_EXEC_RENDER | I915_EXEC_NO_RELOC, 0, target,
		    10ll * 1000 * 1000 * 1000, lbl);
	if (ra == 0) {
		tp = (volatile uint32_t *)gem_mmap_wc(fd, target, 4096, "target");
		if (!tp)
			return die("mmap target");
		if (tp[STORE_OFF / 4] != MAGIC) {
			printf("i915test: %s: readback 0x%08x != 0x%08x\n",
			       tag, tp[STORE_OFF / 4], MAGIC);
			sleep(2);
			printf("i915test: %s: late readback (2s): 0x%08x %s\n",
			       tag, tp[STORE_OFF / 4],
			       tp[STORE_OFF / 4] == MAGIC
			       ? "-- LANDED LATE (wait lied / raced execution)"
			       : "(batch really never executed)");
			return 1;
		}
		printf("i915test: %s: store OK\n", tag);
		return 0;
	}

	/* Rung B — minimal submit: NOP batch, single BO, kernel-placed, relocs allowed
	 * (none supplied). Isolates context+engine+request+vm-bind from softpin. */
	memset(obj, 0, sizeof obj);
	obj[0].handle = batch;
	submit(fd, obj, 1, I915_EXEC_RENDER, NOP_OFF, batch,
	       10ll * 1000 * 1000 * 1000, "nop unpinned");

	/* Rung C — the same NOP batch, softpinned + NO_RELOC. Isolates PINNED. */
	memset(obj, 0, sizeof obj);
	obj[0].handle = batch;
	obj[0].offset = batch_va;
	obj[0].flags  = EXEC_OBJECT_PINNED;
	submit(fd, obj, 1, I915_EXEC_RENDER | I915_EXEC_NO_RELOC, NOP_OFF, batch,
	       10ll * 1000 * 1000 * 1000, "nop softpin");

	printf("i915test: %s: ladder done (see errnos above)\n", tag);
	return 1;
}

/* Deliberate hang: a batch that MI_BATCH_BUFFER_STARTs to its own beginning spins the
 * render engine forever. With hangcheck live, the heartbeat declares a hang within a few
 * periods and resets the engine; the spun request then signals (with an error the fence
 * carries, which GEM_WAIT does not surface) and the 60 s wait completes LONG before its
 * timeout. A GEM_WAIT timeout here = hangcheck/reset did NOT work. */
static int run_hang(int fd)
{
	uint32_t spin = gem_create(fd, 4096);
	uint32_t *sb;
	struct drm_i915_gem_exec_object2 obj[1];
	int pre, post, spun;
	if (!spin)
		return die("GEM_CREATE spin");
	sb = (uint32_t *)gem_mmap_wc(fd, spin, 4096, "spin");
	if (!sb)
		return die("mmap spin");
	/* gen8+ MI_BATCH_BUFFER_START: (0x31<<23)|1, addr lo/hi — jump to our own VA. */
	sb[0] = (0x31u << 23) | 1;
	sb[1] = (uint32_t)SPIN_VA;
	sb[2] = (uint32_t)(SPIN_VA >> 32);

	/* Phase 0 — store BEFORE any hang: does this context execute batches at all?
	 * (Boot #35: a virgin context ran breadcrumbs but no batch, with no hang ever
	 * declared — so "broken from birth" and "broken by reset" must be separated.) */
	reset_stats(fd, "at open");
	pre = run_store(fd, 0x100000ull, "pre-hang store");
	if (pre)
		printf("i915test: VERDICT so far: context broken from BIRTH (no hang was involved)\n");

	/* Phase 1 — the deliberate hang. A healthy spin takes ~seconds (preempt timeout /
	 * heartbeat) before the reset completes it; ~1 ms = it never spun at all. */
	printf("i915test: submitting spin batch -- expecting hangcheck + engine reset...\n");
	memset(obj, 0, sizeof obj);
	obj[0].handle = spin;
	obj[0].offset = SPIN_VA;
	obj[0].flags  = EXEC_OBJECT_PINNED;
	spun = submit(fd, obj, 1, I915_EXEC_RENDER | I915_EXEC_NO_RELOC, 0, spin,
		      60ll * 1000 * 1000 * 1000, "spin wait");
	if (spun)
		return die("hang recovery (GEM_WAIT timed out -- no engine reset?)");
	reset_stats(fd, "after spin");         /* guilty=1 = a real hang was declared */

	/* Phase 2 — store AFTER the hang on the same context (boot #34's failure). */
	post = run_store(fd, 0x300000ull, "post-hang store");
	if (pre == 0 && post == 0) {
		printf("i915test: hang recovered (engine reset)\n");
		return 0;
	}
	reset_stats(fd, "after post-hang store");

	/* Phase 3 — fresh fd = fresh default context + fresh ppGTT on the same engine:
	 * success = per-context/per-vm damage; failure = the engine itself is sick. */
	printf("i915test: retrying store on a FRESH fd (new context + vm)...\n");
	{
		int fd2 = open("/dev/dri/renderD128", O_RDWR);
		if (fd2 < 0)
			return die("open fresh renderD128");
		if (run_store(fd2, 0x100000ull, "fresh-fd store") == 0)
			printf("i915test: fresh-fd store OK -> per-context damage, engine alive\n");
		else {
			reset_stats(fd2, "fresh fd");
			printf("i915test: fresh-fd store FAILED -> the engine itself is sick\n");
		}
		close(fd2);
	}
	return 1;
}

int main(int argc, char **argv)
{
	int fd;

	g_tee = fopen(TEE_PATH, "a");
	printf("i915test: ===== run '%s' (uptime %lld ms) =====%s\n",
	       argc > 1 ? argv[1] : "store", now_ms(),
	       g_tee ? "" : " [tee unavailable -- console only]");

	fd = open("/dev/dri/renderD128", O_RDWR);
	if (fd < 0)
		return die("open renderD128");

	/* Narrate what the driver advertises (iris hard-requires softpin). */
	{
		int chipset = 0, softpin = 0;
		drm_i915_getparam_t gp;
		memset(&gp, 0, sizeof gp);
		gp.param = I915_PARAM_CHIPSET_ID; gp.value = &chipset;
		ioctl(fd, DRM_IOCTL_I915_GETPARAM, &gp);
		gp.param = I915_PARAM_HAS_EXEC_SOFTPIN; gp.value = &softpin;
		ioctl(fd, DRM_IOCTL_I915_GETPARAM, &gp);
		printf("i915test: chipset 0x%x softpin %d\n", chipset, softpin);
		if (!softpin)
			return die("HAS_EXEC_SOFTPIN");
	}

	{
		int r = (argc > 1 && strcmp(argv[1], "hang") == 0)
			? run_hang(fd) : run_store(fd, 0x100000ull, "store");
		close(fd);
		return r;
	}
}

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
 * On a store-submit failure the test walks a DISCRIMINATION LADDER of minimal submits
 * (added after boot #32, where the errno was swallowed and cost a boot):
 *   B: MI_BATCH_BUFFER_END-only batch, single BO, kernel-placed (no softpin, no NO_RELOC)
 *      -> exercises context + engine + request + vm-bind alone.
 *   C: the same NOP batch, softpinned + NO_RELOC -> isolates EXEC_OBJECT_PINNED handling.
 * Every failure prints errno (ioctl() is -1/errno per reterr in libc-glue/syscalls.c).
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdint.h>
#include <drm/drm.h>
#include <drm/i915_drm.h>

#define MAGIC      0xC0DE1915u
#define BATCH_VA   0x100000ull      /* softpin GPU VAs: low, page-aligned, < 4 GiB       */
#define TARGET_VA  0x180000ull      /* (no EXEC_OBJECT_SUPPORTS_48B_ADDRESS needed)      */
#define SPIN_VA    0x200000ull      /* the hang test's self-looping batch                 */
#define STORE_OFF  64ull            /* store lands at TARGET_VA + 64                      */
#define NOP_OFF    2048u            /* MI_BB_END-only batch at +2048 in the batch BO      */

static int die(const char *step)
{
	printf("i915test: FAIL %s (errno=%d)\n", step, errno);
	return 1;
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
		struct drm_i915_gem_wait w;
		memset(&w, 0, sizeof w);
		w.bo_handle  = wait_handle;
		w.timeout_ns = wait_ns;             /* bounded — never open-ended */
		if (ioctl(fd, DRM_IOCTL_I915_GEM_WAIT, &w)) {
			printf("i915test: %s: GEM_WAIT errno=%d (batch never completed?)\n",
			       label, errno);
			return -errno;
		}
	}
	printf("i915test: %s: OK\n", label);
	return 0;
}

/* The gem_exec_store oracle. Returns 0 on "store OK"; on a failed submit runs the ladder. */
static int run_store(int fd)
{
	uint32_t target = gem_create(fd, 4096);
	uint32_t batch  = gem_create(fd, 4096);
	uint32_t *bb;
	volatile uint32_t *tp;
	struct drm_i915_gem_exec_object2 obj[2];
	int ra;
	if (!target || !batch)
		return die("GEM_CREATE");
	printf("i915test: BOs created (target %u, batch %u)\n", target, batch);

	bb = (uint32_t *)gem_mmap_wc(fd, batch, 4096, "batch");
	if (!bb)
		return die("mmap batch");
	/* gen8+ MI_STORE_DWORD_IMM: (0x20<<23)|2, addr lo/hi (ppgtt, bit22=0), data. */
	bb[0] = (0x20u << 23) | 2;
	bb[1] = (uint32_t)(TARGET_VA + STORE_OFF);
	bb[2] = (uint32_t)((TARGET_VA + STORE_OFF) >> 32);
	bb[3] = MAGIC;
	bb[4] = 0x0A << 23;                       /* MI_BATCH_BUFFER_END */
	bb[NOP_OFF / 4] = 0x0A << 23;             /* rung B/C: NOP-only batch */
	printf("i915test: batch written (store 0x%08x -> GPU VA 0x%llx)\n",
	       MAGIC, (unsigned long long)(TARGET_VA + STORE_OFF));

	/* Rung A — the real thing: two BOs, softpinned, store + verify. */
	memset(obj, 0, sizeof obj);
	obj[0].handle = target;                   /* batch LAST (no I915_EXEC_BATCH_FIRST) */
	obj[0].offset = TARGET_VA;
	obj[0].flags  = EXEC_OBJECT_PINNED | EXEC_OBJECT_WRITE;
	obj[1].handle = batch;
	obj[1].offset = BATCH_VA;
	obj[1].flags  = EXEC_OBJECT_PINNED;
	ra = submit(fd, obj, 2, I915_EXEC_RENDER | I915_EXEC_NO_RELOC, 0, target,
		    10ll * 1000 * 1000 * 1000, "store softpin");
	if (ra == 0) {
		tp = (volatile uint32_t *)gem_mmap_wc(fd, target, 4096, "target");
		if (!tp)
			return die("mmap target");
		if (tp[STORE_OFF / 4] != MAGIC) {
			printf("i915test: readback 0x%08x != 0x%08x\n",
			       tp[STORE_OFF / 4], MAGIC);
			return die("readback");
		}
		printf("i915test: store OK\n");
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
	obj[0].offset = BATCH_VA;
	obj[0].flags  = EXEC_OBJECT_PINNED;
	submit(fd, obj, 1, I915_EXEC_RENDER | I915_EXEC_NO_RELOC, NOP_OFF, batch,
	       10ll * 1000 * 1000 * 1000, "nop softpin");

	printf("i915test: ladder done (see errnos above)\n");
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
	if (!spin)
		return die("GEM_CREATE spin");
	sb = (uint32_t *)gem_mmap_wc(fd, spin, 4096, "spin");
	if (!sb)
		return die("mmap spin");
	/* gen8+ MI_BATCH_BUFFER_START: (0x31<<23)|1, addr lo/hi — jump to our own VA. */
	sb[0] = (0x31u << 23) | 1;
	sb[1] = (uint32_t)SPIN_VA;
	sb[2] = (uint32_t)(SPIN_VA >> 32);
	printf("i915test: spin batch submitted — expecting hangcheck + engine reset...\n");

	memset(obj, 0, sizeof obj);
	obj[0].handle = spin;
	obj[0].offset = SPIN_VA;
	obj[0].flags  = EXEC_OBJECT_PINNED;
	if (submit(fd, obj, 1, I915_EXEC_RENDER | I915_EXEC_NO_RELOC, 0, spin,
		   60ll * 1000 * 1000 * 1000, "spin wait"))
		return die("hang recovery (GEM_WAIT timed out — no engine reset?)");
	printf("i915test: hang recovered (engine reset)\n");

	/* The whole point: the GPU must still execute fresh work after the reset. */
	return run_store(fd);
}

int main(int argc, char **argv)
{
	int fd = open("/dev/dri/renderD128", O_RDWR);
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
		int r = (argc > 1 && strcmp(argv[1], "hang") == 0) ? run_hang(fd) : run_store(fd);
		close(fd);
		return r;
	}
}

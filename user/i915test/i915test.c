/*
 * i915test.nxe — the GEM/execbuf oracle (Plan 3 Task 8, mirrors igt's gem_exec_store).
 *
 * Proves the whole GPU-compute path of the unmodified i915 on the Dell with raw ioctls
 * (no libdrm, vendored uapi headers — byte-identical structs/ioctl numbers to the driver):
 *   GEM_CREATE two BOs (batch + target, shmem system pages)
 *   GEM_MMAP_OFFSET + mmap the batch (the i915_drm_node.c mmap-offset resolution)
 *   write a gen8+ MI_STORE_DWORD_IMM batch storing a magic dword into the target
 *   EXECBUFFER2 with both BOs SOFTPINNED (EXEC_OBJECT_PINNED — the same relocation-free
 *     ABI Mesa iris uses) on the default per-file context, render engine
 *   GEM_WAIT on the target (implicit exclusive fence from EXEC_OBJECT_WRITE)
 *   mmap the target and assert the dword — GPU-written memory read back by the CPU.
 *
 * Success marker: "i915test: store OK". Every step dies loudly with its errno otherwise.
 *
 * NOTE deliberate-hang/engine-reset validation (plan Task 8's one-time test) is NOT here:
 * hangcheck is still disabled in the bring-up glue (i915_entry.c), so a hung batch would
 * wedge forever. It comes with the hangcheck re-enable boot.
 */
#include <stdio.h>
#include <string.h>
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
#define STORE_OFF  64ull            /* store lands at TARGET_VA + 64                      */

static int die(const char *step, long err)
{
	printf("i915test: FAIL %s (err=%ld)\n", step, err);
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
		printf("i915test: MMAP_OFFSET(%s) failed\n", what);
		return 0;
	}
	p = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mo.offset);
	if (p == MAP_FAILED) {
		printf("i915test: mmap(%s @0x%llx) failed\n", what, (unsigned long long)mo.offset);
		return 0;
	}
	return p;
}

int main(void)
{
	int fd = open("/dev/dri/renderD128", O_RDWR);
	if (fd < 0)
		return die("open renderD128", fd);

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
			return die("HAS_EXEC_SOFTPIN", 0);
	}

	{
		uint32_t target = gem_create(fd, 4096);
		uint32_t batch  = gem_create(fd, 4096);
		uint32_t *bb;
		volatile uint32_t *tp;
		if (!target || !batch)
			return die("GEM_CREATE", 0);
		printf("i915test: BOs created (target %u, batch %u)\n", target, batch);

		bb = (uint32_t *)gem_mmap_wc(fd, batch, 4096, "batch");
		if (!bb)
			return die("mmap batch", 0);
		/* gen8+ MI_STORE_DWORD_IMM: (0x20<<23)|2, addr lo/hi (ppgtt, bit22=0), data. */
		bb[0] = (0x20u << 23) | 2;
		bb[1] = (uint32_t)(TARGET_VA + STORE_OFF);
		bb[2] = (uint32_t)((TARGET_VA + STORE_OFF) >> 32);
		bb[3] = MAGIC;
		bb[4] = 0x0A << 23;                       /* MI_BATCH_BUFFER_END */
		printf("i915test: batch written (store 0x%08x -> GPU VA 0x%llx)\n",
		       MAGIC, (unsigned long long)(TARGET_VA + STORE_OFF));

		{
			struct drm_i915_gem_exec_object2 obj[2];
			struct drm_i915_gem_execbuffer2 eb;
			long r;
			memset(obj, 0, sizeof obj);
			obj[0].handle = target;               /* batch LAST (no I915_EXEC_BATCH_FIRST) */
			obj[0].offset = TARGET_VA;
			obj[0].flags  = EXEC_OBJECT_PINNED | EXEC_OBJECT_WRITE;
			obj[1].handle = batch;
			obj[1].offset = BATCH_VA;
			obj[1].flags  = EXEC_OBJECT_PINNED;
			memset(&eb, 0, sizeof eb);
			eb.buffers_ptr  = (uintptr_t)obj;
			eb.buffer_count = 2;
			eb.batch_len    = 0;                  /* 0 = to the end of the batch BO */
			eb.flags        = I915_EXEC_RENDER | I915_EXEC_NO_RELOC;
			eb.rsvd1        = 0;                  /* default per-file context */
			r = ioctl(fd, DRM_IOCTL_I915_GEM_EXECBUFFER2, &eb);
			if (r)
				return die("EXECBUFFER2", r);
			printf("i915test: EXECBUFFER2 submitted (rcs, softpin)\n");
		}

		{
			struct drm_i915_gem_wait w;
			long r;
			memset(&w, 0, sizeof w);
			w.bo_handle  = target;
			w.timeout_ns = 10ll * 1000 * 1000 * 1000;   /* 10 s — never open-ended */
			r = ioctl(fd, DRM_IOCTL_I915_GEM_WAIT, &w);
			if (r)
				return die("GEM_WAIT (batch never completed?)", r);
			printf("i915test: GEM_WAIT done (request signalled)\n");
		}

		tp = (volatile uint32_t *)gem_mmap_wc(fd, target, 4096, "target");
		if (!tp)
			return die("mmap target", 0);
		if (tp[STORE_OFF / 4] != MAGIC) {
			printf("i915test: readback 0x%08x != 0x%08x\n", tp[STORE_OFF / 4], MAGIC);
			return die("readback", 0);
		}
		printf("i915test: store OK\n");
	}
	close(fd);
	return 0;
}

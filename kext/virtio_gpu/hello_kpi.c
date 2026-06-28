/*
 * kext/virtio_gpu/hello_kpi.c — P0 build-plumbing proof. A trivial kext entry that links
 * against the LinuxKPI shim (slab + printk) and proves the whole path works: C source with
 * <linux/*.h> headers, compiled with the kext codegen flags, linked via kext64.ld, packed
 * by mknx64, and loaded at boot by loadAllKexts. Replaced by the real virtio_gpu probe in
 * P1.
 */
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/idr.h>

int nkext_init(void) {
	pr_info("virtio_gpu(kpi): hello from the LinuxKPI shim\n");

	/* slab roundtrip */
	char *p = (char *)kzalloc(64, GFP_KERNEL);
	if (!p) {
		pr_err("virtio_gpu(kpi): kzalloc failed\n");
		return -1;
	}
	p[0] = 'O'; p[1] = 'K';
	pr_info("virtio_gpu(kpi): slab roundtrip %c%c, ksize=%zu\n", p[0], p[1], ksize(p));
	kfree(p);

	/* idr roundtrip */
	struct idr idr;
	idr_init(&idr);
	int id = idr_alloc(&idr, (void *)0x1234, 0, 0, GFP_KERNEL);
	pr_info("virtio_gpu(kpi): idr_alloc -> %d, find=%p\n", id, idr_find(&idr, id));
	idr_destroy(&idr);

	pr_info("virtio_gpu(kpi): LinuxKPI primitives OK\n");
	return 0;
}

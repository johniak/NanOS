/*
 * linuxkpi/lkpi_knx.h — declarations of the NanOS kernel facilities the LinuxKPI shim
 * imports (the knx_* ABI from kernel/kexports.def). Wrapped in a conditional extern "C"
 * so the SAME shim sources work both as C (the real .nkext build) and as C++ (the host
 * doctest harness compiles .c with g++). In the kext, these resolve to the kernel export
 * table; on the host, to test shims forwarding to libc.
 */
#ifndef _LINUXKPI_LKPI_KNX_H
#define _LINUXKPI_LKPI_KNX_H

#ifdef __cplusplus
extern "C" {
#endif

void              *knx_malloc(unsigned size);
void               knx_free(void *p);
void               knx_log(const char *s);
unsigned long long knx_uptime_us(void);

/* PCI + MMIO + DMA + IRQ (kexports.def). phys/len are 32-bit: QEMU places virtio-pci BARs
 * and our DMA buffers below 4 GiB, so 32-bit addressing is sufficient. */
int            knx_pci_find(unsigned short vendor, unsigned short device,
                            unsigned char *bus, unsigned char *dev, unsigned char *func);
unsigned int   knx_pci_bar(unsigned char bus, unsigned char dev, unsigned char func, int n);
unsigned int   knx_pci_bar_size(unsigned char bus, unsigned char dev, unsigned char func, int n);
int            knx_pci_bar_is_io(unsigned char bus, unsigned char dev, unsigned char func, int n);
unsigned char  knx_pci_irq(unsigned char bus, unsigned char dev, unsigned char func);
void           knx_pci_enable_bus_master(unsigned char bus, unsigned char dev, unsigned char func);
unsigned int   knx_pci_cfg_read32(unsigned char bus, unsigned char dev, unsigned char func, unsigned char off);
void           knx_pci_cfg_write32(unsigned char bus, unsigned char dev, unsigned char func, unsigned char off, unsigned int v);
void          *knx_map_mmio(unsigned int phys, unsigned int len);
void          *knx_dma_alloc(unsigned int len, unsigned int *phys_out);
void           knx_register_irq(int irq, void (*h)(void *));
int            knx_register_msi(unsigned char bus, unsigned char dev, unsigned char func, void (*h)(void *), void *ctx);
/* Kernel threads for the LinuxKPI kthread/workqueue/timer layer (kpi_kthread.c). A knx thread runs
 * fn(arg) as a scheduler task; the handle drives knx_thread_stop. should_stop is true inside a knx
 * thread whose stop was requested. yield gives up the CPU (worker/timer poll loops). */
void          *knx_thread_spawn(void (*fn)(void *), void *arg, const char *name);
int            knx_thread_should_stop(void);
void           knx_thread_stop(void *handle);
void           knx_thread_yield(void);
/* Run fn() once after the scheduler is up (worker/timer kthreads defer their spawn here). */
void           knx_run_after_scheduler(void (*fn)(void));
/* adopt a kext-owned framebuffer as the system fb: builds /dev/fb0 (+ VT console if the
 * bootloader gave none) and runs a present thread calling `flush` periodically. */
void           knx_fb_set_backing(unsigned long long phys, unsigned int pitch, unsigned int w,
                                  unsigned int h, unsigned char bpp, void (*flush)(void));
void           knx_fb_start_present(void (*flush)(void));
int            knx_boot_fb(unsigned long long *addr, unsigned int *pitch, unsigned int *w,
                           unsigned int *h, unsigned char *bpp);

/* DRM nodes: register the kext's knx_drm_ops table, creating /dev/dri/card0 + renderD128.
 * knx_getpid = the calling process id (per-process drm_file identity). See knx_drm_node.h. */
struct knx_drm_ops;
void           knx_drm_register(const struct knx_drm_ops *ops);
int            knx_getpid(void);

#ifdef __cplusplus
}
#endif

#endif /* _LINUXKPI_LKPI_KNX_H */

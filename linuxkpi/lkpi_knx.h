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
/* top of physical RAM in bytes (highest usable address); LinuxKPI sizes its mem_map against it. */
unsigned long long knx_ram_top(void);
/* Physically-contiguous frame-pool block (bytes rounded to whole pages) at/above min_pa; 0 = no
 * such run. The GEM backing store: the byte heap is capped (512 MiB minus the mem_map), the frame
 * pool is the rest of RAM. min_pa >= 0x60000000 keeps the block identity-visible under process
 * CR3 (above every privatized per-process VA window). Free with the SAME byte count. */
unsigned long long knx_alloc_frames(unsigned long long bytes, unsigned long long min_pa);
void               knx_free_frames(unsigned long long pa, unsigned long long bytes);
unsigned long long knx_heap_free(void);    /* byte-heap telemetry for OOM diagnostics */
unsigned long long knx_heap_total(void);

/* PCI + MMIO + DMA + IRQ (kexports.def). knx_pci_bar / knx_map_mmio are 64-bit: real hardware
 * (e.g. the Dell's Comet Lake GPU) places GTTMMADR/GTT BARs above 4 GiB, and the kernel MMU
 * identity-maps any physical address. DMA buffers still fit below 4 GiB (knx_dma_alloc). */
int            knx_pci_find(unsigned short vendor, unsigned short device,
                            unsigned char *bus, unsigned char *dev, unsigned char *func);
unsigned long long knx_pci_bar(unsigned char bus, unsigned char dev, unsigned char func, int n);
unsigned long long knx_pci_bar_size(unsigned char bus, unsigned char dev, unsigned char func, int n);
int            knx_pci_bar_is_io(unsigned char bus, unsigned char dev, unsigned char func, int n);
unsigned char  knx_pci_irq(unsigned char bus, unsigned char dev, unsigned char func);
void           knx_pci_enable_bus_master(unsigned char bus, unsigned char dev, unsigned char func);
unsigned int   knx_pci_cfg_read32(unsigned char bus, unsigned char dev, unsigned char func, unsigned char off);
void           knx_pci_cfg_write32(unsigned char bus, unsigned char dev, unsigned char func, unsigned char off, unsigned int v);
void          *knx_map_mmio(unsigned long long phys, unsigned long long len);
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
void           knx_thread_msleep(unsigned ms);   /* idle worker/timer sleep (no busy-yield) */
/* Run fn() once after the scheduler is up (worker/timer kthreads defer their spawn here). */
void           knx_run_after_scheduler(void (*fn)(void));
/* Read a whole file through the kernel VFS (request_firmware). buf==0 -> report size in *out_len.
 * Returns 0 on success, <0 (ENOENT/unreadable) otherwise; sets *out_len to the bytes read. */
int            knx_file_read(const char *path, void *buf, unsigned long max, unsigned long *out_len);
/* Append `len` bytes to `path` (create if absent), through the VFS on the writable root. The i915
 * bring-up harness persists boot markers to /nanos/log/i915-boot.txt so a Dell hang leaves a log
 * that survives reboot. Best-effort; returns 0 on success, <0 on error. */
int            knx_file_append(const char *path, const void *buf, unsigned long len);
/* Tee every subsequent printk line to `path` too (bring-up debug: the full drm_dbg trail survives a
 * screen-scroll / hard hang). Pass 0 to stop teeing. Inert unless called — set by the i915 harness. */
void           lkpi_set_log_tee(const char *path);
/* Interrupt-context depth for the log tee. kpi_irq.c bumps this around every inline handler / thread_fn
 * run (lkpi_irq_dispatch + the wait-pump poll harvest). printk's persistent tee consults it: a line
 * emitted while lkpi_in_irq!=0 must NOT touch the VFS/USB directly — knx_file_append takes the
 * non-recursive, IRQ-enabled g_xhciLock, and an MSI landing on a CPU that already holds it (mid file
 * append) would self-deadlock (see Spinlock.h SpinIrqGuard note). Such lines are staged into a RAM ring
 * and drained by lkpi_log_flush() from thread context instead. */
extern volatile int lkpi_in_irq;
/* Drain any tee lines buffered while in interrupt context to the persistent log. MUST be called only
 * from thread context (never holding g_xhciLock): the drm-node ioctl entry calls it every DRM ioctl
 * (~per frame), so buffered IRQ-context drm errors reach the USB log promptly and survive a reboot. */
void           lkpi_log_flush(void);
/* Register a persistent panic sink: the kernel fault handler hands it one preformatted line
 * (vec/rip/rsp/cr2) on a ring-0 CPU exception. The i915 harness points it at its bring-up log so a
 * triple-fault-class panic (e.g. a stack overflow during GT resume) survives a power-cycle even
 * when i915 owns the panel and fbcon is no longer scanned out. Pass 0 to detach. */
void           knx_set_panic_sink(void (*fn)(const char *line));
/* Stack-overflow tripwire (kpi_misc.c). Call lkpi_stack_baseline() once at probe entry to record the
 * kernel stack top; lkpi_stack_deep() returns nonzero once the stack has descended past the redline;
 * lkpi_deep_report() prints the culprit's return address ONCE to the (FS-teed) log. A hot inline
 * primitive uses these to log + defer a runaway recursion before it triple-faults invisibly. */
void           lkpi_stack_baseline(void);
int            lkpi_stack_deep(void);
void           lkpi_deep_report(const char *where, void *ra);
/* NOTE: lkpi_wait_pump / lkpi_spin_probe are NOT declared here. They follow the plain-linkage pattern
 * (declared in <linux/wait.h> / <linux/wait_bit.h> / <linux/completion.h>, not this extern "C" block),
 * so a host C++ TU never sees two decls of different language linkage for them. See kpi_misc.c
 * (lkpi_spin_probe) and kpi_fence.c (lkpi_wait_pump). */
/* Real RCU grace period: block until every other online CPU has passed a quiescent state
 * (LinuxKPI synchronize_rcu). On UP this is a barrier. See Scheduler::rcuSynchronize. */
void           knx_rcu_synchronize(void);
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
/* Short process name (Linux `comm`) for a pid; -1 + a "pid<N>" fallback for an unknown pid. */
int            knx_process_comm(int pid, char *buf, int n);

/* Shim helper (kpi_misc.c, not a kernel export): stamp the shim's `current` task with the
 * calling process's pid + comm so DRM core log lines name the REAL client (glkms/nwm/...)
 * instead of the static placeholder. Call at every drm-node entry point (ioctl/release). */
void           lkpi_set_current_client(int pid);

#ifdef __cplusplus
}
#endif

#endif /* _LINUXKPI_LKPI_KNX_H */

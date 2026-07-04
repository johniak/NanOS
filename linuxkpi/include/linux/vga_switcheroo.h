#ifndef _LKPI_VGA_SWITCHEROO_H
#define _LKPI_VGA_SWITCHEROO_H
static inline void vga_switcheroo_lock_ddc(struct pci_dev *p){ (void)p; }
static inline void vga_switcheroo_unlock_ddc(struct pci_dev *p){ (void)p; }
#endif
#ifndef _LKPI_VGA_DELAYED
#define _LKPI_VGA_DELAYED
static inline void vga_switcheroo_process_delayed_switch(void){}
#endif

#ifndef _LKPI_VGA_PROBE
#define _LKPI_VGA_PROBE
struct pci_dev;
/* single-GPU NanOS: no muxed switcheroo, so probe is never deferred and no handler flags are set. */
static inline int vga_switcheroo_client_probe_defer(struct pci_dev *pdev){ (void)pdev; return 0; }
static inline int vga_switcheroo_handler_flags(void){ return 0; }
#endif

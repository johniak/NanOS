#ifndef _LKPI_VGA_SWITCHEROO_H
#define _LKPI_VGA_SWITCHEROO_H
static inline void vga_switcheroo_lock_ddc(struct pci_dev *p){ (void)p; }
static inline void vga_switcheroo_unlock_ddc(struct pci_dev *p){ (void)p; }
#endif
#ifndef _LKPI_VGA_DELAYED
#define _LKPI_VGA_DELAYED
static inline void vga_switcheroo_process_delayed_switch(void){}
#endif

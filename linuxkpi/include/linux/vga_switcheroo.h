#ifndef _LKPI_VGA_SWITCHEROO_H
#define _LKPI_VGA_SWITCHEROO_H
static inline void vga_switcheroo_lock_ddc(struct pci_dev *p){ (void)p; }
static inline void vga_switcheroo_unlock_ddc(struct pci_dev *p){ (void)p; }
#endif

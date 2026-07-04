/* linuxkpi/include/linux/vgaarb.h — VGA arbitration. NanOS has a single GPU and no VGA routing
 * arbiter, so client registration is accepted and get/put are no-ops. */
#ifndef _LINUXKPI_LINUX_VGAARB_H
#define _LINUXKPI_LINUX_VGAARB_H
struct pci_dev;
#define VGA_RSRC_NONE       0x00
#define VGA_RSRC_LEGACY_IO  0x01
#define VGA_RSRC_LEGACY_MEM 0x02
#define VGA_RSRC_NORMAL_IO  0x04
#define VGA_RSRC_NORMAL_MEM 0x08
static inline int vga_client_register(struct pci_dev *pdev,
	unsigned int (*set_decode)(struct pci_dev *, bool)){ (void)pdev;(void)set_decode; return 0; }
static inline void vga_get_uninterruptible(struct pci_dev *pdev, unsigned int rsrc){ (void)pdev;(void)rsrc; }
static inline void vga_put(struct pci_dev *pdev, unsigned int rsrc){ (void)pdev;(void)rsrc; }
static inline int vga_client_unregister(struct pci_dev *pdev){ (void)pdev; return 0; }
static inline void vga_set_legacy_decoding(struct pci_dev *pdev, unsigned int decodes){ (void)pdev;(void)decodes; }
#endif

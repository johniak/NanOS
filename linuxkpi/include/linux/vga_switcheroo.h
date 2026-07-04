#ifndef _LKPI_VGA_SWITCHEROO_H
#define _LKPI_VGA_SWITCHEROO_H
/* i915_driver.c passes FBINFO_STATE_SUSPENDED to intel_fbdev_set_suspend but reaches the constant
 * only through <linux/vga_switcheroo.h> in our include graph; route <linux/fb.h> here. */
#include <linux/fb.h>
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
#ifndef VGA_SWITCHEROO_CAN_SWITCH_DDC
#define VGA_SWITCHEROO_CAN_SWITCH_DDC  (1 << 0)
#define VGA_SWITCHEROO_NEEDS_EDP_CONFIG (1 << 1)
#endif
#ifndef _LKPI_VGA_STATE
#define _LKPI_VGA_STATE
enum vga_switcheroo_state { VGA_SWITCHEROO_OFF, VGA_SWITCHEROO_ON, VGA_SWITCHEROO_NOT_FOUND };
enum vga_switcheroo_client_id { VGA_SWITCHEROO_UNKNOWN_ID = 0x1000, VGA_SWITCHEROO_IGD = 0, VGA_SWITCHEROO_DIS, VGA_SWITCHEROO_MAX_CLIENTS };
struct vga_switcheroo_client_ops {
	void (*set_gpu_state)(struct pci_dev *dev, enum vga_switcheroo_state state);
	void (*reprobe)(struct pci_dev *dev);
	bool (*can_switch)(struct pci_dev *dev);
	enum vga_switcheroo_client_id (*get_client_id)(struct pci_dev *dev);
};
static inline int vga_switcheroo_register_client(struct pci_dev *dev, const struct vga_switcheroo_client_ops *ops, bool driver_power_control){ (void)dev;(void)ops;(void)driver_power_control; return 0; }
static inline void vga_switcheroo_unregister_client(struct pci_dev *dev){ (void)dev; }
#endif
#endif

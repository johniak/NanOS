#ifndef _LKPI_APERTURE_H
#define _LKPI_APERTURE_H
struct pci_dev; struct platform_device; struct device;
static inline int devm_aperture_acquire_for_platform_device(struct platform_device *p, unsigned long a, unsigned long b){ (void)p;(void)a;(void)b; return 0; }
static inline int aperture_remove_conflicting_devices(unsigned long a, unsigned long b, const char *n){ (void)a;(void)b;(void)n; return 0; }
static inline int aperture_remove_conflicting_pci_devices(struct pci_dev *p, const char *n){ (void)p;(void)n; return 0; }
static inline int aperture_remove_all_conflicting_devices(const char *n){ (void)n; return 0; }
#endif

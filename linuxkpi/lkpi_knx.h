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

#ifdef __cplusplus
}
#endif

#endif /* _LINUXKPI_LKPI_KNX_H */

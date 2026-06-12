/*
 * knx_net.h — the stable C ABI between a network-driver kext (e1000) and the kernel net core.
 * Shared by kernel/NetCore.cpp (definitions, in the KernelExports table) and the kext (caller),
 * so the layout can never drift. A kext never sees the kernel's NetDevice/NetBuf internals — it
 * hands over a flat descriptor and exchanges raw frame bytes.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Driver transmit: the kernel hands a fully-built L2 frame (Ethernet header + payload) for the
// driver to copy into its DMA TX ring and send. Returns 0 on success, <0 on error. The driver
// must NOT retain the pointer past the call (the kernel frees the backing NetBuf afterwards).
typedef int (*knx_tx_fn)(void* drvctx, const void* data, int len);

// Flat device descriptor a driver fills and registers. The kernel copies it into a NetDevice.
struct KnxNetDev {
	char           name[16];   // "eth0"
	unsigned char  mac[6];
	unsigned short mtu;        // 1500 for Ethernet
	knx_tx_fn      tx;
	void*          drvctx;     // opaque, passed back to tx()
};

// Map `len` bytes of device MMIO at physical `phys` into the kernel and return a usable pointer
// (identity-mapped). For a NIC's BAR0 register window.
void* knx_map_mmio(uint32_t phys, uint32_t len);

// Allocate `len` bytes (<= 4096) of physically-contiguous, identity-mapped DMA memory, zeroed.
// Returns the virtual pointer (== physical, RAM is identity-mapped) and writes the physical
// address to *phys_out (for the device's descriptor/ring registers). Returns 0 on OOM.
void* knx_dma_alloc(uint32_t len, uint32_t* phys_out);

// Register a network device. Returns an opaque handle (the kernel NetDevice*) to pass to
// knx_netif_rx, or 0 on failure.
void* knx_add_net_dev(struct KnxNetDev* desc);

// Deliver a received frame to the stack: the kernel copies `len` bytes into a NetBuf and queues
// it for the softirq thread. Safe to call from the driver's IRQ handler (it only enqueues +
// wakes; no stack work runs in IRQ). The driver may recycle its DMA buffer immediately after.
void knx_netif_rx(void* handle, const void* data, int len);

#ifdef __cplusplus
}
#endif

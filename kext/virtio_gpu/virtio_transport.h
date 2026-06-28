/*
 * virtio_transport.h — a hand-built modern virtio-pci transport for the NanOS LinuxKPI
 * module. Replaces Linux's virtio_pci_common.c/virtio_pci_modern.c bus glue with a thin
 * layer over the lifted vp_modern_* register accessors + virtio_ring. Bypasses bus
 * matching: vt_create() builds a virtio_device for a PCI function; the caller then drives
 * the vendored virtio_gpu probe against it.
 */
#ifndef _NANOS_VIRTIO_TRANSPORT_H
#define _NANOS_VIRTIO_TRANSPORT_H

#include <linux/virtio.h>

/* Build a virtio_device for the modern virtio-pci function at (bus,dev,func).
 * Returns the virtio_device (status reset; ACKNOWLEDGE|DRIVER set) or NULL on failure. */
struct virtio_device *vt_create(unsigned char bus, unsigned char dev, unsigned char func);

/* Service a (polled or INTx) interrupt: run vring callbacks for queues with new buffers. */
void vt_interrupt(struct virtio_device *vdev);

#endif /* _NANOS_VIRTIO_TRANSPORT_H */

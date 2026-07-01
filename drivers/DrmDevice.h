/*
 * DrmDevice.h — /dev/dri/card0 + /dev/dri/renderD128. A thin forwarder: the vendored DRM stack
 * lives in virtio_gpu.nkext; the kext registers a knx_drm_ops table and this device routes
 * SYS_ioctl / SYS_mmap(offset) into it. One instance per node kind.
 *
 * Pure dispatch — machine-independent and host-tested (tests/test_drm_node.cpp).
 */
#pragma once
#include "CharDevice.h"
#include <stdint.h>
struct knx_drm_ops;
namespace kernel {
class DrmDevice : public CharDevice {
public:
	DrmDevice(const struct knx_drm_ops* ops, int node) : m_ops(ops), m_node(node) {}
	int read(unsigned, void*, unsigned) override { return -22; }         /* -EINVAL */
	int write(unsigned, const void*, unsigned) override { return -22; }  /* -EINVAL */
	int ioctl(unsigned cmd, void* arg) override;
	int mmapInfo(uint64_t*, unsigned*) override { return -22; }          /* DRM mmap is offset-based only (Task 5) */
	// Offset-aware GEM mmap: resolve a fake offset to a physical range via the kext table.
	int mmapAt(uint64_t off, uint64_t* physOut, unsigned* lenOut) override;
	// On last close for the calling process, release its drm_file + GEM handles (Task 4).
	void close() override;
private:
	const struct knx_drm_ops* m_ops;
	int m_node;
};
// Create /dev/dri/card0 + /dev/dri/renderD128 backed by the kext's op table.
// Kernel build only (defined in KernelExports.cpp, where the /dev SynthFs root lives).
void drmNodesRegister(const struct knx_drm_ops* ops);
}

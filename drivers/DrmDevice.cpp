#include "DrmDevice.h"
#include "knx_drm_node.h"
namespace kernel {

#ifdef NANOS_HOST_TEST
static int curPid() { return 42; }   // host doctest: fixed pid
#else
int syscallCurrentPid();             // kernel/Syscall.cpp: ProcTable::current()->pid
static int curPid() { return syscallCurrentPid(); }
#endif

int DrmDevice::ioctl(unsigned cmd, void* arg) {
	if (!m_ops || !m_ops->ioctl) return -19;   /* -ENODEV */
	return (int) m_ops->ioctl(curPid(), m_node, cmd, arg);
}

int DrmDevice::mmapAt(uint64_t off, uint64_t* physOut, unsigned* lenOut) {
	if (!m_ops || !m_ops->mmap_offset) return -19;   /* -ENODEV */
	uint64_t len64 = 0;
	int r = m_ops->mmap_offset(curPid(), off, physOut, &len64);
	if (r == 0) *lenOut = (unsigned) len64;
	return r;
}

void DrmDevice::close() {
	// Deliberately a no-op. The kext keys drm_files by pid (not fd), and userland routinely
	// churns short-lived descriptors on an in-use node: dup(), Mesa's loader reopening the node
	// by path, libdrm's drmGetDevices2 sniff-open. Releasing here destroyed the process's whole
	// GPU state mid-render — the next drm_file reissued the same GEM handle numbers and Mesa's
	// bufmgr handle table aliased old/new buffers (Dell boot #43: gles2info clear-readback BAD,
	// glkms dead at EGL device setup). The drm_file now dies with the process: procExit/procKill
	// call kernel::drmProcessExit(pid) (KernelExports.cpp). A process that closes its last DRM
	// fd and lives on keeps its drm_file until exit — accepted for the per-process model.
}

}

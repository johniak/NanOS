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
	// No per-fd identity in the current fd layer: on any close we drop this process's drm_file
	// and all its GEM handles. nwm/Mesa hold the node open for their lifetime, so a process that
	// closes a dup'd fd early loses its handles — the recorded follow-on is per-fd identity.
	if (m_ops && m_ops->release) m_ops->release(curPid());
}

}

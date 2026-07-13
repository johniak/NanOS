#include "NwShmDevice.h"
#include "FrameAllocator.h"
#include <arch/mmu.h>
#include <string.h>

namespace kernel {

// Frame-pool floor for surface buffers. Unlike GEM backing (knx_alloc_frames, floor 1.5 GiB),
// these pages are NEVER identity-accessed from process context after allocation — clients and
// the compositor reach them only through their explicit fb-window mappings, and the one kernel
// touch (the scrub below) runs under the kernel CR3, where the full identity map is live. So
// the only constraint is staying above the low-memory/exec-staging clutter; VA_USER_END keeps
// the same hygiene line the DMA allocators use, and — crucially — still exists on a 512 MiB
// QEMU box (a 1.5 GiB floor there can never succeed, which would silently disable the whole
// shm pipeline and fall every client back to pipe commits).
static const uint64_t NWSHM_MIN_PA = arch::VA_USER_END;

NwShmDevice::NwShmDevice()
{
	memset(m_buf, 0, sizeof m_buf);
}

int NwShmDevice::read(unsigned, void*, unsigned)        { return -22; }   // -EINVAL: mmap-only
int NwShmDevice::write(unsigned, const void*, unsigned) { return -22; }
int NwShmDevice::mmapInfo(uint64_t*, unsigned*)         { return -22; }   // must name a token

int NwShmDevice::ioctl(unsigned cmd, void* arg)
{
	nwshm_ioc* io = (nwshm_ioc*) arg;
	if (!io)
		return -22;                                       // -EINVAL
	switch (cmd) {
	case NWSHM_IOC_ALLOC: {
		uint64_t bytes = (io->bytes + 0xFFFull) & ~0xFFFull;
		if (!bytes || bytes > 64ull * 1024 * 1024)        // sanity: one buffer <= 64 MiB
			return -22;
		SpinGuard g(m_lock);
		int slot = -1;
		for (int i = 0; i < MAX_BUFS; i++)
			if (!m_buf[i].phys) { slot = i; break; }
		if (slot < 0)
			return -12;                                   // -ENOMEM: table full
		uint64_t pa = g_frames.allocContigAbove(NWSHM_MIN_PA, bytes >> 12);
		if (!pa)
			return -12;                                   // -ENOMEM: no contiguous run
		{
			// Scrub under the kernel CR3: the caller is a user process, and a frame inside a
			// privatized per-process VA window (anon-mmap/fb windows on big-RAM machines) is
			// NOT identity-visible under its CR3 — same trap as the xhci ringPush #PF.
			uint32_t prev = arch::mmuCurrentDirPhys(), k = arch::mmuKernelDirPhys();
			if (prev != k) arch::mmuLoadDirPhys(k);
			memset((void*) (uintptr_t) pa, 0, (size_t) bytes);   // fresh surface = transparent
			if (prev != k) arch::mmuLoadDirPhys(prev);
		}
		m_buf[slot].phys  = pa;
		m_buf[slot].bytes = bytes;
		io->token = pa;                                   // token == phys, validated on use
		return 0;
	}
	case NWSHM_IOC_FREE: {
		SpinGuard g(m_lock);
		for (int i = 0; i < MAX_BUFS; i++)
			if (m_buf[i].phys && m_buf[i].phys == io->token) {
				g_frames.freeContig(m_buf[i].phys, m_buf[i].bytes >> 12);
				m_buf[i].phys = m_buf[i].bytes = 0;
				return 0;
			}
		return -22;                                       // -EINVAL: unknown token
	}
	}
	return -25;                                           // -ENOTTY
}

int NwShmDevice::mmapAt(uint64_t off, uint64_t* physOut, unsigned* lenOut)
{
	SpinGuard g(m_lock);
	for (int i = 0; i < MAX_BUFS; i++)
		if (m_buf[i].phys && m_buf[i].phys == off) {
			*physOut = m_buf[i].phys;
			*lenOut  = (unsigned) m_buf[i].bytes;
			return 0;
		}
	return -22;                                           // -EINVAL: not a live token
}

}  // namespace kernel

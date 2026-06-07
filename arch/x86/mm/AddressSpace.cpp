#include "AddressSpace.h"
#include "Paging.h"
#include "FrameAllocator.h"   // FRAME_SIZE
#include <string.h>

namespace kernel {

AddressSpace::AddressSpace(const PagingEnv& env) : m_env(env), m_dirPhys(0) {
	m_dirPhys = m_env.allocFrame(m_env.ctx);
	if (m_dirPhys)
		memset(m_env.physToVirt(m_env.ctx, m_dirPhys), 0, FRAME_SIZE);
}

uint32_t* AddressSpace::dir() const {
	return (uint32_t*) m_env.physToVirt(m_env.ctx, m_dirPhys);
}

bool AddressSpace::map(uint32_t va, uint32_t pa, uint32_t flags) {
	if (!m_dirPhys)
		return false;
	uint32_t* pd = dir();
	uint32_t i = pdIndex(va);
	if (!entryPresent(pd[i])) {
		uint32_t ptPhys = m_env.allocFrame(m_env.ctx);
		if (!ptPhys)
			return false;
		memset(m_env.physToVirt(m_env.ctx, ptPhys), 0, FRAME_SIZE);
		// PDE kept permissive; access is enforced at the PTE level.
		pd[i] = makeEntry(ptPhys, PTE_PRESENT | PTE_RW | PTE_USER);
	}
	uint32_t* pt = (uint32_t*) m_env.physToVirt(m_env.ctx, entryAddr(pd[i]));
	pt[ptIndex(va)] = makeEntry(pa, flags);
	return true;
}

void AddressSpace::unmap(uint32_t va) {
	uint32_t* pd = dir();
	uint32_t i = pdIndex(va);
	if (!entryPresent(pd[i]))
		return;
	uint32_t* pt = (uint32_t*) m_env.physToVirt(m_env.ctx, entryAddr(pd[i]));
	pt[ptIndex(va)] = 0;
}

void AddressSpace::adoptKernelDirectory(uint32_t kernelDirPhys, uint32_t userVa) {
	uint32_t* dst = dir();
	uint32_t* src = (uint32_t*) m_env.physToVirt(m_env.ctx, kernelDirPhys);
	for (int i = 0; i < 1024; i++)
		dst[i] = src[i];
	dst[pdIndex(userVa)] = 0;   // drop the user-window PDE -> private PT on next map()
}

bool AddressSpace::mapRange(uint32_t va, uint32_t pa, uint32_t len, uint32_t flags) {
	uint32_t pages = (len + FRAME_SIZE - 1) / FRAME_SIZE;
	for (uint32_t p = 0; p < pages; p++)
		if (!map(va + p * FRAME_SIZE, pa + p * FRAME_SIZE, flags))
			return false;
	return true;
}

uint32_t AddressSpace::translate(uint32_t va) const {
	uint32_t* pd = dir();
	uint32_t i = pdIndex(va);
	if (!entryPresent(pd[i]))
		return 0xFFFFFFFF;
	uint32_t* pt = (uint32_t*) m_env.physToVirt(m_env.ctx, entryAddr(pd[i]));
	uint32_t pte = pt[ptIndex(va)];
	if (!entryPresent(pte))
		return 0xFFFFFFFF;
	return entryAddr(pte) | pageOffset(va);
}

}

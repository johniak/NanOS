#include "AddressSpace.h"
#include "Paging.h"
#include "FrameAllocator.h"   // FRAME_SIZE
#include <string.h>

namespace kernel {

AddressSpace::AddressSpace(const PagingEnv& env) : m_env(env), m_topPhys(0) {
	m_topPhys = m_env.allocFrame(m_env.ctx);
	if (m_topPhys)
		memset(m_env.physToVirt(m_env.ctx, m_topPhys), 0, FRAME_SIZE);
}

uint64_t* AddressSpace::top() const {
	return (uint64_t*) m_env.physToVirt(m_env.ctx, m_topPhys);
}

uint64_t* AddressSpace::nextTable(uint64_t* parent, uint64_t idx, bool create) {
	if (!entryPresent(parent[idx])) {
		if (!create)
			return 0;
		uint64_t f = m_env.allocFrame(m_env.ctx);
		if (!f)
			return 0;
		memset(m_env.physToVirt(m_env.ctx, f), 0, FRAME_SIZE);
		// Intermediate entries kept permissive; access is enforced at the leaf PTE.
		parent[idx] = makeEntry(f, PTE_PRESENT | PTE_RW | PTE_USER);
	}
	return (uint64_t*) m_env.physToVirt(m_env.ctx, entryAddr(parent[idx]));
}

uint64_t* AddressSpace::walk(uint64_t va, bool create) {
	uint64_t* p4 = top();
	uint64_t* p3 = nextTable(p4, pml4Index(va), create); if (!p3) return 0;
	uint64_t* p2 = nextTable(p3, pdptIndex(va), create); if (!p2) return 0;
	uint64_t* p1 = nextTable(p2, pdIndex(va), create);   if (!p1) return 0;
	return &p1[ptIndex(va)];
}

bool AddressSpace::map(uint64_t va, uint64_t pa, uint64_t flags) {
	if (!m_topPhys)
		return false;
	uint64_t* pte = walk(va, true);
	if (!pte)
		return false;
	*pte = makeEntry(pa, flags);
	return true;
}

void AddressSpace::unmap(uint64_t va) {
	uint64_t* pte = walk(va, false);
	if (pte)
		*pte = 0;
}

bool AddressSpace::mapRange(uint64_t va, uint64_t pa, uint64_t len, uint64_t flags) {
	uint64_t pages = (len + FRAME_SIZE - 1) / FRAME_SIZE;
	for (uint64_t p = 0; p < pages; p++)
		if (!map(va + p * FRAME_SIZE, pa + p * FRAME_SIZE, flags))
			return false;
	return true;
}

uint64_t AddressSpace::translate(uint64_t va) const {
	// const walk: read-only, never allocates (create=false). Cast away const for the shared
	// helper, which does not mutate when create is false.
	uint64_t* pte = const_cast<AddressSpace*>(this)->walk(va, false);
	if (!pte || !entryPresent(*pte))
		return 0xFFFFFFFFFFFFFFFFULL;
	return entryAddr(*pte) | pageOffset(va);
}

uint64_t* AddressSpace::privatizeChild(uint64_t* parent, uint64_t idx) {
	if (!entryPresent(parent[idx]))
		return 0;
	uint64_t srcPhys = entryAddr(parent[idx]);
	uint64_t f = m_env.allocFrame(m_env.ctx);
	if (!f)
		return 0;
	uint64_t* dst = (uint64_t*) m_env.physToVirt(m_env.ctx, f);
	uint64_t* src = (uint64_t*) m_env.physToVirt(m_env.ctx, srcPhys);
	for (int i = 0; i < 512; i++)
		dst[i] = src[i];
	// Relink, preserving the original entry's flags (incl. NX) but pointing at the copy.
	parent[idx] = makeEntry(f, parent[idx] & (FLAG_MASK | PTE_NX));
	return dst;
}

uint64_t* AddressSpace::privatizePdPath(uint64_t va) {
	uint64_t* p4 = top();
	uint64_t* p3 = privatizeChild(p4, pml4Index(va)); if (!p3) return 0;
	uint64_t* p2 = privatizeChild(p3, pdptIndex(va)); if (!p2) return 0;
	return p2;   // the private PD; pdIndex(va) selects the leaf-PT slot in it
}

void AddressSpace::adoptKernelDirectory(uint64_t kernelTopPhys, uint64_t userVa) {
	uint64_t* dst = top();
	uint64_t* src = (uint64_t*) m_env.physToVirt(m_env.ctx, kernelTopPhys);
	for (int i = 0; i < 512; i++)
		dst[i] = src[i];        // share the whole kernel half by value
	dropPde(userVa);            // privatize the path to the user window + drop its PD entry
}

void AddressSpace::dropPde(uint64_t va) {
	uint64_t* pd = privatizePdPath(va);
	if (pd)
		pd[pdIndex(va)] = 0;    // private PD now: clearing this entry affects only this space
}

void AddressSpace::freeUserWindow(uint64_t userVa) {
	uint64_t* p4 = top();
	if (!entryPresent(p4[pml4Index(userVa)])) return;
	uint64_t* p3 = (uint64_t*) m_env.physToVirt(m_env.ctx, entryAddr(p4[pml4Index(userVa)]));
	if (!entryPresent(p3[pdptIndex(userVa)])) return;
	uint64_t* p2 = (uint64_t*) m_env.physToVirt(m_env.ctx, entryAddr(p3[pdptIndex(userVa)]));
	uint64_t pdi = pdIndex(userVa);
	if (!entryPresent(p2[pdi])) return;
	uint64_t ptPhys = entryAddr(p2[pdi]);
	uint64_t* pt = (uint64_t*) m_env.physToVirt(m_env.ctx, ptPhys);
	for (int e = 0; e < 512; e++)
		if (entryPresent(pt[e]))
			m_env.freeFrame(m_env.ctx, entryAddr(pt[e]));   // the mapped user frame
	m_env.freeFrame(m_env.ctx, ptPhys);                     // the page table itself
	p2[pdi] = 0;
}

bool AddressSpace::copyUserWindowFrom(const AddressSpace& src, uint64_t userVa) {
	uint64_t* s4 = src.top();
	if (!entryPresent(s4[pml4Index(userVa)])) return true;
	uint64_t* s3 = (uint64_t*) m_env.physToVirt(m_env.ctx, entryAddr(s4[pml4Index(userVa)]));
	if (!entryPresent(s3[pdptIndex(userVa)])) return true;
	uint64_t* s2 = (uint64_t*) m_env.physToVirt(m_env.ctx, entryAddr(s3[pdptIndex(userVa)]));
	uint64_t pdi = pdIndex(userVa);
	if (!entryPresent(s2[pdi])) return true;   // nothing mapped -> nothing to copy (success)
	uint64_t* spt = (uint64_t*) m_env.physToVirt(m_env.ctx, entryAddr(s2[pdi]));
	// First VA covered by this PD entry (2 MiB-aligned), kept canonical.
	uint64_t base = canonical(((uint64_t) pml4Index(userVa) << 39)
	                        | ((uint64_t) pdptIndex(userVa) << 30)
	                        | (pdi << 21));
	for (int e = 0; e < 512; e++) {
		if (!entryPresent(spt[e]))
			continue;
		uint64_t srcPa = entryAddr(spt[e]);
		uint64_t flags = spt[e] & (FLAG_MASK | PTE_NX);
		uint64_t newPa = m_env.allocFrame(m_env.ctx);
		if (!newPa)
			return false;   // OOM: caller tears the partial copy down
		memcpy(m_env.physToVirt(m_env.ctx, newPa),
				m_env.physToVirt(m_env.ctx, srcPa), FRAME_SIZE);
		if (!map(base | ((uint64_t) e << 12), newPa, flags)) {
			m_env.freeFrame(m_env.ctx, newPa);   // table alloc failed: undo this frame, fail
			return false;
		}
	}
	return true;
}

}

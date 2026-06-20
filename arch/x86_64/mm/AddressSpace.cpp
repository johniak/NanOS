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
		// Intermediate entries kept permissive; access is enforced at the leaf PTE. PTE_PRIV marks
		// this table as a per-process private allocation (teardown frees it; never re-privatized).
		parent[idx] = makeEntry(f, PTE_PRESENT | PTE_RW | PTE_USER | PTE_PRIV);
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

bool AddressSpace::mapRangeHuge(uint64_t va, uint64_t pa, uint64_t len, uint64_t flags) {
	if (!m_topPhys)
		return false;
	const uint64_t HP = 0x200000;                       // 2 MiB
	uint64_t pages = (len + HP - 1) / HP;
	for (uint64_t i = 0; i < pages; i++) {
		uint64_t v = va + i * HP, a = pa + i * HP;
		uint64_t* p4 = top();
		uint64_t* p3 = nextTable(p4, pml4Index(v), true); if (!p3) return false;
		uint64_t* p2 = nextTable(p3, pdptIndex(v), true); if (!p2) return false;
		p2[pdIndex(v)] = makeEntry(a, flags | PTE_PS);   // 2 MiB leaf at the PD level
	}
	return true;
}

bool AddressSpace::mapRange(uint64_t va, uint64_t pa, uint64_t len, uint64_t flags) {
	uint64_t pages = (len + FRAME_SIZE - 1) / FRAME_SIZE;
	for (uint64_t p = 0; p < pages; p++)
		if (!map(va + p * FRAME_SIZE, pa + p * FRAME_SIZE, flags))
			return false;
	return true;
}

uint64_t AddressSpace::translate(uint64_t va) const {
	// Read-only descent (create=false). Stops at a 2 MiB huge PD entry (PTE_PS) if present, else
	// descends to the 4 KiB PTE. Cast away const for the shared helpers (they don't mutate here).
	AddressSpace* self = const_cast<AddressSpace*>(this);
	uint64_t* p4 = self->top();
	uint64_t* p3 = self->nextTable(p4, pml4Index(va), false); if (!p3) return 0xFFFFFFFFFFFFFFFFULL;
	uint64_t* p2 = self->nextTable(p3, pdptIndex(va), false); if (!p2) return 0xFFFFFFFFFFFFFFFFULL;
	uint64_t pde = p2[pdIndex(va)];
	if (!entryPresent(pde)) return 0xFFFFFFFFFFFFFFFFULL;
	if (pde & PTE_PS) return entryAddr(pde) | (va & 0x1FFFFF);   // 2 MiB huge page
	uint64_t* p1 = self->nextTable(p2, pdIndex(va), false); if (!p1) return 0xFFFFFFFFFFFFFFFFULL;
	uint64_t pte = p1[ptIndex(va)];
	if (!entryPresent(pte)) return 0xFFFFFFFFFFFFFFFFULL;
	return entryAddr(pte) | pageOffset(va);
}

uint64_t* AddressSpace::privatizeChild(uint64_t* parent, uint64_t idx) {
	if (!entryPresent(parent[idx]))
		return 0;
	// Already a per-process private copy? Re-privatizing would allocate a fresh frame and orphan
	// (leak) the current one — the bug behind the per-fork frame leak. Idempotent: reuse it.
	if (parent[idx] & PTE_PRIV)
		return (uint64_t*) m_env.physToVirt(m_env.ctx, entryAddr(parent[idx]));
	uint64_t srcPhys = entryAddr(parent[idx]);
	uint64_t f = m_env.allocFrame(m_env.ctx);
	if (!f)
		return 0;
	uint64_t* dst = (uint64_t*) m_env.physToVirt(m_env.ctx, f);
	uint64_t* src = (uint64_t*) m_env.physToVirt(m_env.ctx, srcPhys);
	for (int i = 0; i < 512; i++)
		dst[i] = src[i] & ~PTE_PRIV;   // the copied children are SHARED aliases of src's children
		                               // until individually privatized — they must not look private
		                               // (else teardown would free a table this AS only aliases).
	// Relink, preserving the original entry's flags (incl. NX) + mark the copy private (PTE_PRIV)
	// so a later privatize is a no-op and teardown frees it.
	parent[idx] = makeEntry(f, (parent[idx] & (FLAG_MASK | PTE_NX)) | PTE_PRIV);
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
		dst[i] = src[i] & ~PTE_PRIV;   // share the kernel half by value, but the SHARED kernel tables
		                               // must NOT look private: clear PTE_PRIV so privatizeChild forks
		                               // them on first user write and teardown never frees them.
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

void AddressSpace::freeUserTables() {
	// Free this process's PRIVATE intermediate tables. PTE_PRIV (set by privatizeChild/nextTable,
	// cleared on shared kernel-half entries) marks a per-process table. freeUserWindow already
	// freed the leaf PTs + pages, so we free only the PDs + PDPTs here. We free the PD FRAME
	// itself but do NOT descend into its entries: any kernel-aliased PTs it still references are
	// owned by the real kernel PML4 and must survive.
	uint64_t* p4 = top();
	for (int i = 0; i < 512; i++) {
		if (!(p4[i] & PTE_PRIV))                // shared kernel PDPT (or empty) — leave it
			continue;
		uint64_t* p3 = (uint64_t*) m_env.physToVirt(m_env.ctx, entryAddr(p4[i]));
		for (int j = 0; j < 512; j++)
			if (p3[j] & PTE_PRIV)               // a private PD (its PTs already freed above)
				m_env.freeFrame(m_env.ctx, entryAddr(p3[j]));
		m_env.freeFrame(m_env.ctx, entryAddr(p4[i]));   // the private PDPT itself
		p4[i] = 0;
	}
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

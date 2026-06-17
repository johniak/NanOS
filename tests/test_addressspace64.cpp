#include "doctest.h"
#include "AddressSpace.h"
#include "Paging.h"
#include "FrameAllocator.h"   // FRAME_SIZE
#include <cstdint>
#include <cstdlib>
#include <cstring>

using namespace kernel;

// A fake physical-memory environment: one big arena, frames handed out as arena offsets.
// "Physical address" == offset into the arena, so physToVirt is just arena+pa. Frame 0 is
// never handed out (0 stays the OOM sentinel).
struct FakeMem {
	char* arena;
	uint64_t next;     // next phys offset to hand out
	int allocCount;
	int cap;           // max frames (for OOM tests); <=0 means unlimited
};
static uint64_t fakeAlloc(void* c) {
	FakeMem* m = (FakeMem*) c;
	if (m->cap > 0 && m->allocCount >= m->cap)
		return 0;      // OOM
	uint64_t pa = m->next;
	m->next += FRAME_SIZE;
	m->allocCount++;
	return pa;
}
static void fakeFree(void* c, uint64_t) { ((FakeMem*) c)->allocCount--; }
static void* fakeP2V(void* c, uint64_t pa) { return ((FakeMem*) c)->arena + pa; }

static const uint64_t NOPE = 0xFFFFFFFFFFFFFFFFULL;   // translate() sentinel

static FakeMem* makeMem(int cap = 0) {
	FakeMem* m = new FakeMem();
	m->arena = (char*) calloc(1, 0x200000);   // 2 MiB arena (room for the 4-level walk)
	m->next = FRAME_SIZE;                       // first frame is 0x1000, never 0
	m->allocCount = 0;
	m->cap = cap;
	return m;
}
static PagingEnv envOf(FakeMem* m) {
	PagingEnv e = { fakeAlloc, fakeFree, fakeP2V, m };
	return e;
}
// White-box: walk PML4->PT via the fake env and read the raw entry for a mapped VA.
static uint64_t rawPte(FakeMem* m, AddressSpace& as, uint64_t va) {
	uint64_t* p4 = (uint64_t*) fakeP2V(m, as.directoryPhys());
	if (!entryPresent(p4[pml4Index(va)])) return 0;
	uint64_t* p3 = (uint64_t*) fakeP2V(m, entryAddr(p4[pml4Index(va)]));
	if (!entryPresent(p3[pdptIndex(va)])) return 0;
	uint64_t* p2 = (uint64_t*) fakeP2V(m, entryAddr(p3[pdptIndex(va)]));
	if (!entryPresent(p2[pdIndex(va)])) return 0;
	uint64_t* p1 = (uint64_t*) fakeP2V(m, entryAddr(p2[pdIndex(va)]));
	return p1[ptIndex(va)];
}

TEST_CASE("constructor allocates a zeroed PML4") {
	FakeMem* m = makeMem();
	AddressSpace as(envOf(m));
	CHECK(as.directoryPhys() != 0);
	CHECK(m->allocCount == 1);                  // just the PML4
}

TEST_CASE("map then translate round-trips, preserving the page offset") {
	FakeMem* m = makeMem();
	AddressSpace as(envOf(m));
	CHECK(as.map(0x400000, 0xAB000, PTE_PRESENT | PTE_RW));
	CHECK(as.translate(0x400000) == 0xAB000u);
	CHECK(as.translate(0x400123) == 0xAB123u);  // same page, offset preserved
}

TEST_CASE("first map allocates PDPT+PD+PT; a neighbour reuses them") {
	FakeMem* m = makeMem();
	AddressSpace as(envOf(m));
	int dirOnly = m->allocCount;                // 1 (PML4)
	as.map(0x400000, 0x10000, PTE_PRESENT | PTE_RW);
	CHECK(m->allocCount == dirOnly + 3);        // PDPT + PD + PT
	int afterFirst = m->allocCount;
	as.map(0x401000, 0x11000, PTE_PRESENT | PTE_RW);   // same PT
	CHECK(m->allocCount == afterFirst);         // no new table
	as.map(0x800000, 0x12000, PTE_PRESENT | PTE_RW);   // 8 MiB: different PD entry -> new PT only
	CHECK(m->allocCount == afterFirst + 1);
}

TEST_CASE("a VA under a different PDPT entry allocates a fresh PD+PT") {
	FakeMem* m = makeMem();
	AddressSpace as(envOf(m));
	as.map(0x400000, 0x10000, PTE_PRESENT | PTE_RW);   // PDPT 0 (+PDPT,PD,PT)
	int after = m->allocCount;
	as.map(0x40000000ULL, 0x20000, PTE_PRESENT | PTE_RW);  // 1 GiB: PDPT entry 1 -> new PD + PT
	CHECK(m->allocCount == after + 2);
	CHECK(as.translate(0x40000000ULL) == 0x20000u);
}

TEST_CASE("mapRange maps every page in the span") {
	FakeMem* m = makeMem();
	AddressSpace as(envOf(m));
	CHECK(as.mapRange(0x400000, 0x100000, 0x3000, PTE_PRESENT | PTE_RW));
	CHECK(as.translate(0x400000) == 0x100000u);
	CHECK(as.translate(0x401000) == 0x101000u);
	CHECK(as.translate(0x402000) == 0x102000u);
}

TEST_CASE("unmap makes a VA untranslatable") {
	FakeMem* m = makeMem();
	AddressSpace as(envOf(m));
	as.map(0x400000, 0xAB000, PTE_PRESENT | PTE_RW);
	as.unmap(0x400000);
	CHECK(as.translate(0x400000) == NOPE);
}

TEST_CASE("translate returns the sentinel for unmapped addresses") {
	FakeMem* m = makeMem();
	AddressSpace as(envOf(m));
	CHECK(as.translate(0x400000) == NOPE);          // no tables at all
	as.map(0x400000, 0xAB000, PTE_PRESENT | PTE_RW);
	CHECK(as.translate(0x800000) == NOPE);          // tables exist elsewhere, this slot empty
}

TEST_CASE("PTE flags (including NX) propagate from map") {
	FakeMem* m = makeMem();
	AddressSpace as(envOf(m));
	as.map(0x400000, 0xAB000, PTE_PRESENT | PTE_RW | PTE_USER | PTE_NX);
	uint64_t e = rawPte(m, as, 0x400000);
	CHECK((e & FLAG_MASK) == (PTE_PRESENT | PTE_RW | PTE_USER));
	CHECK((e & PTE_NX) != 0);
	CHECK(entryAddr(e) == 0xAB000u);
}

TEST_CASE("map fails when a table cannot be allocated (OOM)") {
	FakeMem* m = makeMem(1);                    // only the PML4 fits
	AddressSpace as(envOf(m));
	CHECK(as.directoryPhys() != 0);
	CHECK(!as.map(0x400000, 0xAB000, PTE_PRESENT | PTE_RW));   // PDPT alloc returns 0
}

TEST_CASE("freeUserWindow releases the user PT + its frames, clears the PD entry") {
	FakeMem* m = makeMem();
	AddressSpace as(envOf(m));
	uint64_t f1 = fakeAlloc(m), f2 = fakeAlloc(m), f3 = fakeAlloc(m);   // 3 user frames
	as.map(0x400000, f1, PTE_PRESENT | PTE_RW | PTE_USER);              // + PDPT,PD,PT
	as.map(0x401000, f2, PTE_PRESENT | PTE_RW | PTE_USER);
	as.map(0x402000, f3, PTE_PRESENT | PTE_RW | PTE_USER);
	int before = m->allocCount;                                        // PML4+PDPT+PD+PT + 3 frames = 7

	as.freeUserWindow(0x400000);
	CHECK(m->allocCount == before - 4);                                // 3 frames + 1 PT freed
	CHECK(as.translate(0x400000) == NOPE);                             // PD entry cleared
}

TEST_CASE("freeUserWindow is a no-op when the user window was never mapped") {
	FakeMem* m = makeMem();
	AddressSpace as(envOf(m));
	int before = m->allocCount;
	as.freeUserWindow(0x400000);
	CHECK(m->allocCount == before);
}

TEST_CASE("adoptKernelDirectory shares the kernel half, privatizes the user window") {
	FakeMem* m = makeMem();
	// "kernel" space: map a kernel page (0x100000) and a page in the user window (0x400000).
	AddressSpace kern(envOf(m));
	kern.map(0x100000, 0x100000, PTE_PRESENT | PTE_RW);
	kern.map(0x400000, 0x222000, PTE_PRESENT | PTE_RW);

	AddressSpace proc(envOf(m));                          // same arena
	proc.adoptKernelDirectory(kern.directoryPhys(), 0x400000);

	// Kernel half shared (by value): a kernel VA still translates through the copied tables.
	CHECK(proc.translate(0x100000) == 0x100000u);
	// User window dropped: 0x400000 is now unmapped in the process.
	CHECK(proc.translate(0x400000) == NOPE);

	// Mapping the user window allocates exactly one fresh private PT (the PML4->PDPT->PD path
	// was already privatized during adopt)...
	int before = m->allocCount;
	proc.map(0x400000, 0xAB000, PTE_PRESENT | PTE_RW | PTE_USER);
	CHECK(m->allocCount == before + 1);
	CHECK(proc.translate(0x400000) == 0xAB000u);
	// ...and the kernel's own user-window mapping is untouched (private PT).
	CHECK(kern.translate(0x400000) == 0x222000u);
}

TEST_CASE("copyUserWindowFrom duplicates each user page into fresh frames") {
	FakeMem* m = makeMem();
	// Parent space with two user pages holding distinguishable bytes.
	AddressSpace parent(envOf(m));
	uint64_t pf1 = fakeAlloc(m), pf2 = fakeAlloc(m);
	memset(fakeP2V(m, pf1), 0xAA, FRAME_SIZE);
	memset(fakeP2V(m, pf2), 0xBB, FRAME_SIZE);
	parent.map(0x400000, pf1, PTE_PRESENT | PTE_RW | PTE_USER);
	parent.map(0x402000, pf2, PTE_PRESENT | PTE_RW | PTE_USER);

	AddressSpace child(envOf(m));
	child.adoptKernelDirectory(parent.directoryPhys(), 0x400000);   // share + privatize window
	int before = m->allocCount;
	CHECK(child.copyUserWindowFrom(parent, 0x400000));
	// Two fresh frames + one fresh PT for the child's user window.
	CHECK(m->allocCount == before + 3);

	// Child sees the same VAs but backed by DIFFERENT physical frames.
	uint64_t c1 = child.translate(0x400000) & PAGE_MASK;
	uint64_t c2 = child.translate(0x402000) & PAGE_MASK;
	CHECK(c1 != (NOPE & PAGE_MASK));
	CHECK(c1 != pf1);
	CHECK(c2 != pf2);
	// Contents copied; flags preserved.
	CHECK(((unsigned char*) fakeP2V(m, c1))[0] == 0xAA);
	CHECK(((unsigned char*) fakeP2V(m, c2))[0] == 0xBB);
	CHECK((rawPte(m, child, 0x400000) & FLAG_MASK) == (PTE_PRESENT | PTE_RW | PTE_USER));

	// Writing the child's copy does not disturb the parent's frame (isolation).
	memset(fakeP2V(m, c1), 0xCC, FRAME_SIZE);
	CHECK(((unsigned char*) fakeP2V(m, pf1))[0] == 0xAA);
}

TEST_CASE("copyUserWindowFrom on an empty user window copies nothing") {
	FakeMem* m = makeMem();
	AddressSpace parent(envOf(m));
	AddressSpace child(envOf(m));
	child.adoptKernelDirectory(parent.directoryPhys(), 0x400000);
	int before = m->allocCount;
	CHECK(child.copyUserWindowFrom(parent, 0x400000));
	CHECK(m->allocCount == before);
}

TEST_CASE("dropPde privatizes the path and unmaps just that PD-entry region") {
	FakeMem* m = makeMem();
	AddressSpace kern(envOf(m));
	kern.map(0x100000, 0x100000, PTE_PRESENT | PTE_RW);   // PD entry 0
	kern.map(0x400000, 0x222000, PTE_PRESENT | PTE_RW);   // PD entry 2

	AddressSpace proc(envOf(m));
	proc.adoptKernelDirectory(kern.directoryPhys(), 0x800000);  // privatize a DIFFERENT window
	// 0x400000 is still shared at this point.
	CHECK(proc.translate(0x400000) == 0x222000u);
	proc.dropPde(0x400000);                                     // now drop it too
	CHECK(proc.translate(0x400000) == NOPE);
	// The neighbouring kernel page under the SAME PD stays mapped (only one PD entry cleared).
	CHECK(proc.translate(0x100000) == 0x100000u);
	// The kernel's own mapping is untouched.
	CHECK(kern.translate(0x400000) == 0x222000u);
}

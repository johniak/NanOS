#include "doctest.h"
#include "AddressSpace.h"
#include "Paging.h"
#include "FrameAllocator.h"   // FRAME_SIZE
#include <cstdint>
#include <cstdlib>
#include <cstring>

using namespace kernel;

// A fake physical-memory environment: one big arena, frames handed out as
// arena offsets. "Physical address" == offset into the arena, so physToVirt is
// just arena+pa. Frame 0 is never handed out (0 stays the OOM sentinel).
struct FakeMem {
	char* arena;
	uint32_t next;     // next phys offset to hand out
	int allocCount;
	int cap;           // max frames (for OOM tests); <=0 means unlimited
};
static uint32_t fakeAlloc(void* c) {
	FakeMem* m = (FakeMem*) c;
	if (m->cap > 0 && m->allocCount >= m->cap)
		return 0;      // OOM
	uint32_t pa = m->next;
	m->next += FRAME_SIZE;
	m->allocCount++;
	return pa;
}
static void fakeFree(void* c, uint32_t) { ((FakeMem*) c)->allocCount--; }
static void* fakeP2V(void* c, uint32_t pa) { return ((FakeMem*) c)->arena + pa; }

static FakeMem* makeMem(int cap = 0) {
	FakeMem* m = new FakeMem();
	m->arena = (char*) calloc(1, 0x100000);   // 1 MiB arena
	m->next = FRAME_SIZE;                       // first frame is 0x1000, never 0
	m->allocCount = 0;
	m->cap = cap;
	return m;
}
static PagingEnv envOf(FakeMem* m) {
	PagingEnv e = { fakeAlloc, fakeFree, fakeP2V, m };
	return e;
}
// White-box: read the raw PTE flags for a mapped VA via the fake env.
static uint32_t pteFlags(FakeMem* m, AddressSpace& as, uint32_t va) {
	uint32_t* pd = (uint32_t*) fakeP2V(m, as.directoryPhys());
	uint32_t* pt = (uint32_t*) fakeP2V(m, entryAddr(pd[pdIndex(va)]));
	return pt[ptIndex(va)] & 0xFFF;
}

TEST_CASE("constructor allocates a zeroed page directory") {
	FakeMem* m = makeMem();
	AddressSpace as(envOf(m));
	CHECK(as.directoryPhys() != 0);
	CHECK(m->allocCount == 1);                  // just the directory
}

TEST_CASE("map then translate round-trips, preserving the page offset") {
	FakeMem* m = makeMem();
	AddressSpace as(envOf(m));
	CHECK(as.map(0x400000, 0xAB000, PTE_PRESENT | PTE_RW));
	CHECK(as.translate(0x400000) == 0xAB000u);
	CHECK(as.translate(0x400123) == 0xAB123u);  // same page, offset preserved
}

TEST_CASE("a second VA in the same 4MB region reuses the page table") {
	FakeMem* m = makeMem();
	AddressSpace as(envOf(m));
	as.map(0x400000, 0x10000, PTE_PRESENT | PTE_RW);
	int afterFirst = m->allocCount;             // dir + 1 PT = 2
	as.map(0x401000, 0x11000, PTE_PRESENT | PTE_RW);
	CHECK(m->allocCount == afterFirst);         // no new PT
	as.map(0x800000, 0x12000, PTE_PRESENT | PTE_RW);  // different PD entry
	CHECK(m->allocCount == afterFirst + 1);     // exactly one new PT
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
	CHECK(as.translate(0x400000) == 0xFFFFFFFFu);
}

TEST_CASE("translate returns the sentinel for unmapped addresses") {
	FakeMem* m = makeMem();
	AddressSpace as(envOf(m));
	CHECK(as.translate(0x400000) == 0xFFFFFFFFu);   // no PD entry at all
	as.map(0x400000, 0xAB000, PTE_PRESENT | PTE_RW);
	CHECK(as.translate(0x800000) == 0xFFFFFFFFu);   // PD entry exists elsewhere, this PT slot empty
}

TEST_CASE("PTE flags propagate from map") {
	FakeMem* m = makeMem();
	AddressSpace as(envOf(m));
	as.map(0x400000, 0xAB000, PTE_PRESENT | PTE_RW | PTE_USER);
	CHECK(pteFlags(m, as, 0x400000) == (PTE_PRESENT | PTE_RW | PTE_USER));
}

TEST_CASE("map fails when a page table cannot be allocated (OOM)") {
	FakeMem* m = makeMem(1);                    // only the directory fits
	AddressSpace as(envOf(m));
	CHECK(as.directoryPhys() != 0);
	CHECK(!as.map(0x400000, 0xAB000, PTE_PRESENT | PTE_RW));   // PT alloc returns 0
}

TEST_CASE("freeUserWindow releases the user PT + its frames, clears the PDE") {
	FakeMem* m = makeMem();
	AddressSpace as(envOf(m));
	uint32_t f1 = fakeAlloc(m), f2 = fakeAlloc(m), f3 = fakeAlloc(m);   // 3 user frames
	as.map(0x400000, f1, PTE_PRESENT | PTE_RW | PTE_USER);              // + 1 page table
	as.map(0x401000, f2, PTE_PRESENT | PTE_RW | PTE_USER);
	as.map(0x402000, f3, PTE_PRESENT | PTE_RW | PTE_USER);
	int before = m->allocCount;                                        // dir + 3 frames + PT = 5

	as.freeUserWindow(0x400000);
	CHECK(m->allocCount == before - 4);                                // 3 frames + 1 PT freed
	CHECK(as.translate(0x400000) == 0xFFFFFFFFu);                      // PDE cleared
}

TEST_CASE("freeUserWindow is a no-op when the user window was never mapped") {
	FakeMem* m = makeMem();
	AddressSpace as(envOf(m));
	int before = m->allocCount;
	as.freeUserWindow(0x400000);
	CHECK(m->allocCount == before);
}

TEST_CASE("copyUserWindowFrom duplicates each user page into fresh frames") {
	FakeMem* m = makeMem();
	// Parent space with two user pages holding distinguishable bytes.
	AddressSpace parent(envOf(m));
	uint32_t pf1 = fakeAlloc(m), pf2 = fakeAlloc(m);
	memset(fakeP2V(m, pf1), 0xAA, FRAME_SIZE);
	memset(fakeP2V(m, pf2), 0xBB, FRAME_SIZE);
	parent.map(0x400000, pf1, PTE_PRESENT | PTE_RW | PTE_USER);
	parent.map(0x402000, pf2, PTE_PRESENT | PTE_RW | PTE_USER);

	AddressSpace child(envOf(m));
	child.adoptKernelDirectory(parent.directoryPhys(), 0x400000);   // share kernel half
	int before = m->allocCount;
	child.copyUserWindowFrom(parent, 0x400000);
	// Two fresh frames + one fresh page table for the child's user window.
	CHECK(m->allocCount == before + 3);

	// Child sees the same VAs but backed by DIFFERENT physical frames.
	uint32_t c1 = child.translate(0x400000) & 0xFFFFF000;
	uint32_t c2 = child.translate(0x402000) & 0xFFFFF000;
	CHECK(c1 != 0xFFFFF000u);
	CHECK(c1 != pf1);
	CHECK(c2 != pf2);
	// Contents copied; flags preserved.
	CHECK(((unsigned char*) fakeP2V(m, c1))[0] == 0xAA);
	CHECK(((unsigned char*) fakeP2V(m, c2))[0] == 0xBB);
	CHECK(pteFlags(m, child, 0x400000) == (PTE_PRESENT | PTE_RW | PTE_USER));

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
	child.copyUserWindowFrom(parent, 0x400000);
	CHECK(m->allocCount == before);
}

TEST_CASE("adoptKernelDirectory shares the kernel half, privatizes the user window") {
	FakeMem* m = makeMem();
	// "kernel" space: map a kernel page (PDE 0) and a page in the user window's
	// PDE 1 range above the window (e.g. 0x700000) to prove the shared PT is kept.
	AddressSpace kern(envOf(m));
	kern.map(0x100000, 0x100000, PTE_PRESENT | PTE_RW);   // PDE 0
	kern.map(0x400000, 0x222000, PTE_PRESENT | PTE_RW);   // PDE 1 (kernel's view)

	AddressSpace proc(envOf(m));                          // same arena
	proc.adoptKernelDirectory(kern.directoryPhys(), 0x400000);

	// Kernel half shared: a kernel VA still translates through the copied PDE.
	CHECK(proc.translate(0x100000) == 0x100000u);
	// User window PDE cleared: 0x400000 is now unmapped in the process.
	CHECK(proc.translate(0x400000) == 0xFFFFFFFFu);

	// Mapping the user window allocates exactly one fresh private page table...
	int before = m->allocCount;
	proc.map(0x400000, 0xAB000, PTE_PRESENT | PTE_RW | PTE_USER);
	CHECK(m->allocCount == before + 1);
	CHECK(proc.translate(0x400000) == 0xAB000u);
	CHECK(pteFlags(m, proc, 0x400000) == (PTE_PRESENT | PTE_RW | PTE_USER));
	// ...and the kernel's own user-window mapping is untouched (private PT).
	CHECK(kern.translate(0x400000) == 0x222000u);
}

# x86_64 Plan 7 — Sprzątanie MI pod LP64 (-Wconversion)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Domknąć w większości mechaniczny przegląd LP64 kodu MI: tam, gdzie 32-bitowość
jest *wprost zakodowana* (`unsigned`/`int` dla adresów, rozmiarów, offsetów plików, numerów
bloków), poszerzyć typ na `uintptr_t` / `size_t` / `uint64_t` dokładnie według §4 specu. Robotę
prowadzi `-Wconversion` na buildzie x86_64 **plus** istniejący host-test suite (natywnie LP64) jako
siatka bezpieczeństwa — błędy są dziś **latentne** (kernel ILP32, testy rzadko przepychają adresy
>4 GiB), więc każdą poprawkę domykamy host-testem, który *na wąskim typie by się urwał*.

**Architecture:** Migracja-zastąpienie (spec §0.1, §1.1). To plan **wyłącznie MI** (katalogi
`init kernel mm fs lib drivers`); `make check-arch` MUSI zostać zielony — żadna wewnętrzność x86 nie
może przeciec do MI. Głównym poligonem jest obraz `nanos-test` (natywny LP64, 8-bajtowe wskaźniki),
więc większość zadań nie potrzebuje QEMU. Plan jest **częściowo równoległy** z Planami 2–6 (każdy z
nich zrobił minimalny podzbiór, którego potrzebował); Plan 7 *kończy przegląd* i **włącza bramkę
`-Wconversion`** na buildzie x86_64. Może lądować przyrostowo (patrz `## Zależności`).

**Tech Stack:** doctest (host, `nanos-test`, natywny LP64), `make test` + bramka pokrycia ≥90%,
toolchain `x86_64-elf` w `nanos-build`, `-Wconversion`/`-Werror=conversion`. Stałe typów: adresy /
wskaźniki-jako-liczby → `uintptr_t`; rozmiary / liczności → `size_t`; adresy fizyczne / offsety
plików → `uint64_t` (`off_t` = 64-bit ze znakiem); wartość zwracana syscalla → `long`.

**SHARED NAMING CONTRACT (spójne z Planami 1–6, 8):**
- `kernelSyscall` przyjmuje finalną sygnaturę
  `long kernelSyscall(long nr, uintptr_t a0..a5, arch::TrapFrame*)` — współdzieloną przez
  `<arch/syscall.h>` (deklaracja), `kernel/SyscallDispatch.cpp` (definicja) i każdy `arch/*/cpu/
  syscall_*.cpp` (wywołanie). Na LP64 `uintptr_t == unsigned long == 8 B`, więc x86_64
  `syscall`-trap (Plan 6) przekazuje pełne `rdi/rsi/rdx/r10/r8/r9` bez obcięcia, a `rax` (zwrot)
  mieści 64-bitowy adres (np. `mmap`).
- Literały okien VA są już w `arch/mmu.h` (przeniesione w Planie 3) — `kernel/Exec.cpp` konsumuje
  `arch::mmuStageBase()` / `arch::mmuStageCap()`, **nie** redefiniuje literałów.
- Sygnatury kontraktów `arch/mmu.h` operujące na adresach są w Planie 3 poszerzone na
  `uint64_t`(phys) / `uintptr_t`(va); zadania 12–13 *konsumują* je (zdejmują rzutowania `(uint32_t)`),
  nie definiują.

**Reference:** spec `docs/superpowers/specs/2026-06-15-x86_64-migration-analysis.md`, §4 (cała:
„Krytyczne" pkt 1–9 + „Istotne"), §7 kamień 7, §6 (wiersz „MI cleanup LP64").

---

## File Structure

| Plik | Pozycja §4 | Akcja |
|---|---|---|
| `Makefile` + `arch/x86_64/arch.mk` + `arch/x86/arch.mk` | narzędzie + bramka | Modify (cele `convcheck`/`_convcheck`, knob `KWFLAGS`) |
| `mm/FrameAllocator.{h,cpp}` | Krytyczne #1 | Modify (adresy fizyczne → `uint64_t`) |
| `mm/Heap.{h,cpp}` | Istotne | Modify (`size` → `size_t`, offsety hooka → `uintptr_t`) |
| `mm/memory_manager.cpp` | Krytyczne #2 | Modify (`size_t`, `heapPutHex` 64-bit) |
| `drivers/Console.{h,cpp}` | Krytyczne #5 | Modify (przeciążenia `writeHex(uint64_t)` / `itoa(uint64_t,int)`) |
| `drivers/Framebuffer.cpp` | Krytyczne #6 | Modify (arytmetyka offsetu → `size_t`) |
| `lib/String.h`, `lib/List.h` | Istotne | Modify (`length`/`capacity`/`count` → `size_t`) |
| `drivers/BlockDevice.h` (+`RamBlockDevice`, ext) | Istotne | Modify (`lba` → `uint64_t`; ext4 48-bit udokumentowane) |
| `fs/Vfs.h` (+`FileSystem`/`Syscalls`/`SynthFs`/`Fb0Device`) | Istotne | Modify (`mmapInfo` `physOut` → `uint64_t*`) |
| `kernel/Syscall.h` (+`Syscall.cpp`) | Krytyczne #8/#9 | Modify (`Fd`/`lseek`/`LinuxStat` → `off_t` 64-bit) |
| `arch/include/arch/syscall.h` + `kernel/SyscallDispatch.cpp` | Krytyczne #3 | Modify (args → `uintptr_t`, zwrot → `long`) |
| `kernel/Exec.cpp` | Krytyczne #7 | Modify (`STAGE_*` z `arch/mmu.h`, `entry`/`esp` → `uintptr_t`) |
| `kernel/Kernel.cpp` | Krytyczne #4 | Modify (rzutowania MMIO/BAR/FB, `freeCount`/`top`) |

---

## Task 1: Narzędzie — per-plikowy check `-Wconversion` + knob `KWFLAGS`

Wprowadza powtarzalny sposób weryfikacji LP64 **bez** pełnego linku kernela x86_64 (który zależy od
Planów 2–6): składniowy check pojedynczego pliku MI toolchainem `x86_64-elf` z `-Wconversion`,
kompilowany z kopii na case-sensitive FS kontenera (omija pułapkę `string.h`↔`String.h`). Knob
`KWFLAGS` (pusty dziś) trzyma flagę bramki — Task 14 ją zapala.

**Files:**
- Modify: `Makefile` (sekcja host: cel `convcheck`; sekcja kontenera: `_convcheck`; `CXXFLAGS`)
- Modify: `arch/x86/arch.mk`, `arch/x86_64/arch.mk` (domyślne `KWFLAGS`)

- [ ] **Step 1: Dodać `KWFLAGS ?=` do obu arch.mk**

W `arch/x86/arch.mk`, po `KARCHFLAGS ?=` (dodanym w Planie 1):
```makefile
# -Wconversion gate flags. Empty for i686 (kept green during the transition); the x86_64
# arch.mk turns the gate on (Plan 7) so LP64 truncations fail the build.
KWFLAGS ?=
```
W `arch/x86_64/arch.mk`, po `KARCHFLAGS ?= ...` — **na razie pusty** (Task 14 ustawi wartość):
```makefile
# -Wconversion gate (Plan 7 enables it in its final task). Empty until the MI sweep is clean,
# so intermediate Plan-7 commits don't fail the x86_64 build on not-yet-swept files.
KWFLAGS ?=
```

- [ ] **Step 2: Dopiąć `$(KWFLAGS)` do `CXXFLAGS`**

`Makefile:415` — zamień:
```makefile
CXXFLAGS=-ffreestanding -nostdlib -nostdinc++ $(KINCLUDES) -Wall --no-exceptions --no-rtti -fno-sized-deallocation -fno-leading-underscore $(KARCHFLAGS) $(KOPTFLAGS)
```
na (dodane `$(KWFLAGS)`):
```makefile
CXXFLAGS=-ffreestanding -nostdlib -nostdinc++ $(KINCLUDES) -Wall --no-exceptions --no-rtti -fno-sized-deallocation -fno-leading-underscore $(KARCHFLAGS) $(KWFLAGS) $(KOPTFLAGS)
```

- [ ] **Step 3: Dodać host-owy cel `convcheck` (sekcja HOST, obok `check-arch`, ~`Makefile:368`)**
```makefile
# Per-file LP64 truncation check with the x86_64 toolchain. MI files include only the arch
# CONTRACTS (arch/include) + the freestanding <...> headers, so a single MI TU compiles without
# the rest of arch/x86_64 (filled in by Plans 2-6). Usage:  make convcheck FILE=mm/FrameAllocator.cpp
.PHONY: convcheck
convcheck:
	$(DOCKER_RUN) make ARCH=x86_64 _convcheck FILE=$(FILE)
```

- [ ] **Step 4: Dodać kontener-owy cel `_convcheck` (sekcja CONTAINER, po `_compile`, ~`Makefile:441`)**
```makefile
# Syntax-only -Wconversion check of ONE MI source, compiled from a case-sensitive copy (the
# macOS bind mount collides string.h/String.h). Reports warnings for $(FILE) only; sibling
# headers' own warnings are addressed by their own Plan-7 tasks. The final gate (KWFLAGS) is
# what turns these into hard errors across the whole build.
_convcheck:
	@rm -rf $(KSRC) && mkdir -p $(KSRC) && \
	 tar -cf - --exclude=.git --exclude=disk --exclude=bin --exclude=iso --exclude=coverage -C /src . | tar -xf - -C $(KSRC)
	cd $(KSRC) && $(CXX) -fsyntax-only -Wconversion $(CXXFLAGS) $(FILE)
```

- [ ] **Step 5: Zbudować obraz buildowy (jeśli arch.mk/Makefile się zmieniły) i potwierdzić, że i686 nie regresuje**

Run: `make build && make check-arch`
Expected: kernel i686 buduje się (KWFLAGS puste = no-op); `OK: MI layer is arch-clean.`

- [ ] **Step 6: Smoke test celu `convcheck` na nietkniętym pliku**

Run: `make convcheck FILE=mm/FrameAllocator.cpp`
Expected: kompilacja przechodzi; wypisze ostrzeżenia `-Wconversion` dla `FrameAllocator.cpp`
(to jest właśnie lista do posprzątania w Tasku 2 — narzędzie działa).

- [ ] **Step 7: Commit**
```bash
git add Makefile arch/x86/arch.mk arch/x86_64/arch.mk
git commit -m "build: add per-file -Wconversion convcheck target + KWFLAGS gate knob"
```

---

## Task 2: FrameAllocator — adresy fizyczne na `uint64_t` (§4 #1)

Alokator jest dziś zaszyty na 32-bit: `init/markRange*/alloc/free/freeCount` operują na `uint32_t`,
a bitmapa pokrywa 4 GiB. Na 64-bit kernelu adres fizyczny >4 GiB gubi górne bity. Poszerzamy całą
arytmetykę adresową na `uint64_t` i podnosimy świadomy limit bitmapy (16 GiB; bitmapa 512 KiB w
`.bss`) tak, by ramki powyżej 4 GiB były adresowalne.

**Files:**
- Modify: `mm/FrameAllocator.h`, `mm/FrameAllocator.cpp`
- Modify: `tests/test_frameallocator.cpp` (test >4 GiB)

- [ ] **Step 1: Test — alokacja ponad 4 GiB nie obcina górnych bitów (RED)**

Dopisz do `tests/test_frameallocator.cpp`:
```cpp
TEST_CASE("allocates a frame above 4 GiB without truncating the high bits (LP64)") {
	// topOfRam = 8 GiB; free a single frame at 5 GiB and allocate it back.
	const uint64_t FIVE_GIB = 5ull * 1024 * 1024 * 1024;
	FrameAllocator* fa = fresh((uint32_t) 0 /* placeholder, replaced below */);
	delete fa;
	fa = new FrameAllocator();
	fa->init(8ull * 1024 * 1024 * 1024);          // 8 GiB
	fa->markRangeFree(FIVE_GIB, 0x1000);          // exactly one frame at 5 GiB
	uint64_t a = fa->alloc();
	CHECK(a == FIVE_GIB);                          // NOT (uint32_t) FIVE_GIB == 0x40000000
	CHECK((a >> 32) != 0);                         // genuinely a 64-bit address
	delete fa;
}
```
Uwaga: helper `fresh(uint32_t)` z istniejącego pliku zmieni sygnaturę w Step 3 na `uint64_t`; po
poprawce uprość powyższy test do `FrameAllocator* fa = fresh(8ull*1024*1024*1024);`.

Run: `make test`
Expected: **RED** — `a == 0x40000000` (obcięte) i/lub kompilacja zawodzi (init nie przyjmuje 8 GiB,
bo `m_frameCount` i bitmapa są 32-bit / za małe).

- [ ] **Step 2: Poszerzyć nagłówek `mm/FrameAllocator.h`**

Zamień typy. `FRAME_SIZE` zostaje 4096 (jako `uint64_t` dla czystej arytmetyki):
```cpp
const uint64_t FRAME_SIZE = 4096;

class FrameAllocator {
public:
	void init(uint64_t topOfRam);
	void markRangeFree(uint64_t base, uint64_t len);
	void markRangeUsed(uint64_t base, uint64_t len);

	uint64_t alloc();          // first-free frame's physical address; 0 = OOM
	void free(uint64_t pa);    // return a frame to the pool

	bool isUsed(uint64_t frameIndex) const;
	uint64_t freeCount() const;

private:
	// Conscious cap: the static bitmap covers 16 GiB of physical space (512 KiB in .bss).
	// Enough for any QEMU/target RAM we boot; raising it is a one-line change here. Multiboot1
	// itself can't report regions past 4 GiB (spec §2.1) — this is the allocator-side headroom.
	static const uint64_t MAX_PHYS   = 16ull * 1024 * 1024 * 1024;
	static const uint64_t MAX_FRAMES = MAX_PHYS / FRAME_SIZE;       // 1<<22 frames
	static const uint64_t BITMAP_WORDS = MAX_FRAMES / 32;          // 131072 words = 512 KiB
	uint32_t m_bitmap[BITMAP_WORDS];                               // 32 frames per word
	uint64_t m_frameCount;

	void set(uint64_t i);
	void clear(uint64_t i);
	bool test(uint64_t i) const;
	uint64_t findFirstFree() const;   // returns m_frameCount if none
};
```

- [ ] **Step 3: Przepisać `mm/FrameAllocator.cpp` na `uint64_t`**

Wszystkie indeksy/adresy → `uint64_t`. Bitmapa indeksowana `i >> 5` / `i & 31` (słowa 32-bit),
maska `1u << (i & 31)` (32-bit, OK). `init` przycina `m_frameCount` do `MAX_FRAMES`:
```cpp
void FrameAllocator::init(uint64_t topOfRam) {
	m_frameCount = topOfRam / FRAME_SIZE;
	if (m_frameCount > MAX_FRAMES)         // clamp to what the bitmap can represent
		m_frameCount = MAX_FRAMES;
	memset(m_bitmap, 0xFF, sizeof(m_bitmap));   // everything used until freed
}

void FrameAllocator::set(uint64_t i)        { m_bitmap[i >> 5] |= (1u << (i & 31)); }
void FrameAllocator::clear(uint64_t i)      { m_bitmap[i >> 5] &= ~(1u << (i & 31)); }
bool FrameAllocator::test(uint64_t i) const { return (m_bitmap[i >> 5] >> (i & 31)) & 1u; }

void FrameAllocator::markRangeFree(uint64_t base, uint64_t len) {
	uint64_t start = (base + FRAME_SIZE - 1) / FRAME_SIZE;   // round up
	uint64_t end = (base + len) / FRAME_SIZE;                // round down
	if (end > m_frameCount) end = m_frameCount;
	for (uint64_t f = start; f < end; f++) clear(f);
}

void FrameAllocator::markRangeUsed(uint64_t base, uint64_t len) {
	uint64_t start = base / FRAME_SIZE;                        // round down
	uint64_t end = (base + len + FRAME_SIZE - 1) / FRAME_SIZE; // round up
	if (end > m_frameCount) end = m_frameCount;
	for (uint64_t f = start; f < end; f++) set(f);
}

uint64_t FrameAllocator::findFirstFree() const {
	for (uint64_t w = 0; w < BITMAP_WORDS; w++) {
		if (m_bitmap[w] != 0xFFFFFFFFu) {
			for (uint64_t b = 0; b < 32; b++)
				if (!((m_bitmap[w] >> b) & 1u)) {
					uint64_t f = w * 32 + b;
					return f < m_frameCount ? f : m_frameCount;
				}
		}
	}
	return m_frameCount;
}

uint64_t FrameAllocator::alloc() {
	uint64_t f = findFirstFree();
	if (f >= m_frameCount) return 0;   // OOM (frame 0 is reserved low memory, so 0 is unambiguous)
	set(f);
	return f * FRAME_SIZE;
}

void FrameAllocator::free(uint64_t pa)            { clear(pa / FRAME_SIZE); }
bool FrameAllocator::isUsed(uint64_t frameIndex) const { return test(frameIndex); }

uint64_t FrameAllocator::freeCount() const {
	uint64_t n = 0;
	for (uint64_t f = 0; f < m_frameCount; f++) if (!test(f)) n++;
	return n;
}
```

- [ ] **Step 4: Uprościć/dopiąć test i uruchomić (GREEN)**

Zmień helper na `static FrameAllocator* fresh(uint64_t top)` i uprość nowy test do
`FrameAllocator* fa = fresh(8ull*1024*1024*1024);`. (Uwaga: `arch::mmuInitKernel`/`FrameAllocator`
żyją w `.bss`; tu test alokuje na stosie/heapie — wszystkie istniejące przypadki dalej działają.)

Run: `make test`
Expected: **GREEN** — `a == 5 GiB`, `(a>>32)!=0`; pozostałe 8 przypadków bez regresji; pokrycie ≥90%.

- [ ] **Step 5: `-Wconversion` czysty dla pliku**

Run: `make convcheck FILE=mm/FrameAllocator.cpp`
Expected: brak ostrzeżeń `-Wconversion` wskazujących na `FrameAllocator.*`.

- [ ] **Step 6: Commit**
```bash
git add mm/FrameAllocator.h mm/FrameAllocator.cpp tests/test_frameallocator.cpp
git commit -m "mm: FrameAllocator uses 64-bit physical addresses (LP64), 16 GiB bitmap cap"
```

---

## Task 3: Heap — rozmiary `size_t`, offsety hooka `uintptr_t` (§4 Istotne)

`Heap` to silnik `malloc`. Jego API bierze `unsigned size` (obcięcie >4 GiB), a wewnętrzne offsety
są 32-bitowe — ale jest to **przemyślane** (linki to OFFSETY od bazy areny, nie wskaźniki, więc kod
jest poprawny i na 32-bit kernelu i na 64-bit host-teście). Poszerzamy *publiczne* rozmiary na
`size_t` i offsety hooka korupcji na `uintptr_t`; offsety wewnętrzne (32-bit) zostają — areny kernela
nigdy nie przekraczają 4 GiB i to jest świadomy, udokumentowany wybór (komentarz w nagłówku).

**Files:**
- Modify: `mm/Heap.h`, `mm/Heap.cpp`
- Modify: `tests/test_memory_manager.cpp` (asercja na typ rozmiaru)

- [ ] **Step 1: Test — `alloc`/`freeBytes` przyjmują `size_t` i liczą duże rozmiary bez obcięcia (RED)**

Dopisz do `tests/test_memory_manager.cpp` (operuje na pojedynczym `Heap` nad lokalną areną):
```cpp
TEST_CASE("Heap size API is size_t-wide (no 32-bit truncation of the request)") {
	static char arena[1 << 20];
	kernel::Heap h; h.init(arena, sizeof arena);
	// A request whose low 32 bits are small but which is huge as size_t must NOT wrap to a
	// tiny allocation. On a 1 MiB arena it simply fails (returns 0), never succeeds by truncation.
	size_t huge = (size_t) 1 << 40;            // 1 TiB; (unsigned) huge == 0
	CHECK(h.alloc(huge) == nullptr);
	// totalBytes/freeBytes are size_t and report the real arena, not a narrowed value.
	CHECK(h.totalBytes() >= (sizeof arena) - 64);
}
```
Run: `make test`
Expected: **RED** — `(unsigned) huge == 0`, więc dziś `alloc(0)` może zwrócić mały blok zamiast 0.

- [ ] **Step 2: Poszerzyć `mm/Heap.h`**

Zamień rozmiarowe `unsigned` na `size_t`, a hook korupcji na `uintptr_t` (offsety areny):
```cpp
	void init(void* base, size_t size);
	void* alloc(size_t size);              // 0 on out-of-memory
	void free(void* ptr);
	void* realloc(void* ptr, size_t size);

	size_t freeBytes() const;              // total free payload bytes (for tests/stats)
	size_t totalBytes() const { return m_end; }   // arena size (for /proc/meminfo)

	typedef void (*CorruptFn)(const char* what, uintptr_t off, uintptr_t hdr, uintptr_t ftr);
	static void onCorruption(CorruptFn fn) { s_corrupt = fn; }
```
W komentarzu nagłówkowym zachowaj/uściślij notę: *internal links remain 32-bit OFFSETS from the
arena base — kernel arenas never exceed 4 GiB; this is a deliberate, documented bound.* Pola
`m_end` / `m_freeHead` zostają `unsigned` (offsety wewnętrzne).

- [ ] **Step 3: Dostosować `mm/Heap.cpp`**

Sygnatury `init/alloc/realloc/freeBytes` na `size_t`. Wewnątrz, gdzie rozmiar trafia do offsetowej
arytmetyki, rzutuj jawnie na `unsigned` z komentarzem (świadomy 32-bit offset):
```cpp
void* Heap::alloc(size_t size) {
	unsigned want = (unsigned) size;          // arena offsets are 32-bit by design (see Heap.h)
	if ((size_t) want != size) return 0;      // request too large for a 32-bit-offset arena -> OOM
	/* ... reszta bez zmian ... */
}
```
(Analogicznie `realloc`; `init` przyjmuje `size_t size`, liczy `m_end` jak dotąd po rzutowaniu z tą
samą strażą.) `freeBytes()` zwraca `size_t` (suma `unsigned` -> bez obcięcia).

- [ ] **Step 4: Run + convcheck**

Run: `make test`
Expected: **GREEN** — duży request → `nullptr`; istniejące testy `Heap`/`memory_manager` bez regresji.

Run: `make convcheck FILE=mm/Heap.cpp`
Expected: brak ostrzeżeń `-Wconversion` dla `Heap.*`.

- [ ] **Step 5: Commit**
```bash
git add mm/Heap.h mm/Heap.cpp tests/test_memory_manager.cpp
git commit -m "mm: Heap public size API is size_t; corruption-hook offsets uintptr_t"
```

---

## Task 4: memory_manager — `size_t` + 64-bitowy `heapPutHex` (§4 #2)

`malloc/calloc/realloc` rzutują `size` na `(unsigned)` (obcięcie >4 GiB); `calloc` mnoży `nmeb*size`
jako `unsigned` (przepełnienie); `heapPutHex` drukuje tylko 32 bity. Po Tasku 3 `Heap` przyjmuje
`size_t`, więc tu zdejmujemy rzutowania i naprawiamy hex + sygnaturę hooka.

**Files:**
- Modify: `mm/memory_manager.cpp`
- Modify: `mm/memory_manager.h` (sygnatury `heapInit`/`heapTotalBytes`/`heapFreeBytes`)
- Modify: `tests/test_memory_manager.cpp` (calloc overflow)

- [ ] **Step 1: Test — `calloc` nie owija się na `size_t` przepełnieniu (RED)**

```cpp
TEST_CASE("calloc rejects a multiplication that would overflow (no silent tiny alloc)") {
	// nmeb*size overflows 32 bits but not size_t; calloc must not hand back a small buffer.
	void* p = calloc((size_t) 0x100000, (size_t) 0x100000);   // 0x100000 * 0x100000 = 1 TiB
	CHECK(p == nullptr);
}
```
(Host: `calloc` to nasza wersja z `memory_manager.cpp`, podlinkowana przez shimy.)

Run: `make test`
Expected: **RED** — dziś `(unsigned)nmeb*(unsigned)size == 0`, `alloc(0)` może oddać niezerowy wskaźnik.

- [ ] **Step 2: Naprawić alokatory + hex w `mm/memory_manager.cpp`**

```cpp
static void heapPutHex(uintptr_t v) {
	arch::consolePutChar('0'); arch::consolePutChar('x');
	// Print every nibble of a pointer-wide value (8 hex on i686, 16 on x86_64).
	for (int i = (int) (sizeof(uintptr_t) * 8) - 4; i >= 0; i -= 4) {
		int d = (int) ((v >> i) & 0xF);
		arch::consolePutChar((char) (d < 10 ? '0' + d : 'a' + d - 10));
	}
}
static void heapPanic(const char* what, uintptr_t off, uintptr_t hdr, uintptr_t ftr) {
	arch::cpuDisableInterrupts();
	heapPutStr("\n*** HEAP CORRUPTION: "); heapPutStr(what);
	heapPutStr(" off="); heapPutHex(off);
	heapPutStr(" hdr="); heapPutHex(hdr);
	heapPutStr(" ftr="); heapPutHex(ftr);
	heapPutStr(" ***\n");
	for (;;) arch::cpuHalt();
}

void heapInit(void* base, size_t size) {
	g_heap.init(base, size);
	kernel::Heap::onCorruption(heapPanic);
}
size_t heapTotalBytes(void) { return g_heap.totalBytes(); }
size_t heapFreeBytes(void)  { return g_heap.freeBytes(); }

void *malloc(size_t size)              { return g_heap.alloc(size); }
void *calloc(size_t nmeb, size_t size) {
	size_t total;
	if (__builtin_mul_overflow(nmeb, size, &total)) return 0;   // reject the overflow
	void* ptr = g_heap.alloc(total);
	if (ptr) memset(ptr, 0, total);
	return ptr;
}
void  free(void *ptr)                  { g_heap.free(ptr); }
void *realloc(void *ptr, size_t size)  { return g_heap.realloc(ptr, size); }
```
(`heapPanic` jest typu `Heap::CorruptFn`, więc jego argumenty muszą być `uintptr_t` — zgodnie z
Taskiem 3.)

- [ ] **Step 3: Zaktualizować `mm/memory_manager.h`**

Zsynchronizuj deklaracje: `void heapInit(void* base, size_t size);`,
`size_t heapTotalBytes(void);`, `size_t heapFreeBytes(void);` (sprawdź też, że `<stddef.h>` /
`size_t` są dostępne — plik już używa `size_t` dla `malloc`).

- [ ] **Step 4: Run + convcheck**

Run: `make test`
Expected: **GREEN** — calloc overflow → `nullptr`; reszta bez regresji.

Run: `make convcheck FILE=mm/memory_manager.cpp`
Expected: brak ostrzeżeń `-Wconversion` dla `memory_manager.*`.

> Uwaga: `Kernel.cpp` woła `heapTotalBytes()/heapFreeBytes()` i dzieli przez 1024 do `unsigned`
> (`/proc/meminfo`). Po zmianie zwrotu na `size_t` powstaje zwężenie — naprawiane w Tasku 13.

- [ ] **Step 5: Commit**
```bash
git add mm/memory_manager.cpp mm/memory_manager.h tests/test_memory_manager.cpp
git commit -m "mm: malloc/calloc/realloc are size_t-clean; heapPutHex prints pointer-wide values"
```

---

## Task 5: Console — przeciążenia `writeHex(uint64_t)` / `itoa(uint64_t,int)` (§4 #5)

`writeHex(int)` / `itoa(int,int)` nie potrafią wypisać 64-bitowego adresu (np. BAR-a PCI). Dodajemy
przeciążenia 64-bitowe; istniejące `int`-owe zostają (kompatybilność wsteczna, hex bajtów). Format
liczb jest testowalny bezpośrednio przez `itoa(uint64_t,int)` (statyczny bufor), więc nie trzeba
przechwytywać stdout.

**Files:**
- Modify: `drivers/Console.h`, `drivers/Console.cpp`
- Create: `tests/test_console.cpp` (+ dodać do `TEST_MODULES`? — `Console.cpp` już jest w TEST_MODULES;
  dodać tylko plik testu do `TEST_SRCS`/`tests/`)

- [ ] **Step 1: Test — `itoa(uint64_t,16)` formatuje pełne 64 bity (RED)**

Utwórz `tests/test_console.cpp`:
```cpp
#include "doctest.h"
#include "Console.h"
#include <cstring>
using namespace kernel;

TEST_CASE("Console::itoa(uint64_t,16) prints all 64 bits, not the low 32") {
	CHECK(strcmp(Console::itoa((uint64_t) 0x1ABCDEF012345678ull, 16), "1abcdef012345678") == 0);
	CHECK(strcmp(Console::itoa((uint64_t) 0x100000000ull, 16), "100000000") == 0);  // bit 32 set
}
```
(`tests/*.cpp` są zbierane przez `TEST_SRCS` w Makefile — patrz `_test`; nowy plik trafia do builda
automatycznie, jeśli `TEST_SRCS` to glob `tests/*.cpp`. Jeśli to jawna lista, dopisz
`tests/test_console.cpp`.)

Run: `make test`
Expected: **RED** — brak przeciążenia `itoa(uint64_t,int)` ⇒ błąd kompilacji (lub wpadnięcie w
`int` i obcięcie).

- [ ] **Step 2: Dodać deklaracje w `drivers/Console.h`**
```cpp
		static void writeHex(int hex);
		static void writeHex(uint64_t hex);        // 64-bit: prints all significant nibbles
		...
		static char *itoa(int i,int base);
		static char *itoa(uint64_t v,int base);    // 64-bit unsigned formatter
```
(Dodaj `#include <stdint.h>` na górze, obok `#include <string.h>`.)

- [ ] **Step 3: Zaimplementować w `drivers/Console.cpp`**
```cpp
char* Console::itoa(uint64_t value, int base) {
	static char result[72] = { 0 };          // up to 64 binary digits + NUL
	if (base < 2 || base > 36) { *result = '\0'; return result; }
	char* ptr = result;
	uint64_t v = value;
	do {
		uint64_t q = v / (uint64_t) base;
		unsigned digit = (unsigned) (v - q * (uint64_t) base);
		*ptr++ = "0123456789abcdefghijklmnopqrstuvwxyz"[digit];
		v = q;
	} while (v);
	*ptr-- = '\0';
	// reverse in place (the loop produced least-significant digit first)
	char* p1 = result;
	while (p1 < ptr) { char t = *ptr; *ptr-- = *p1; *p1++ = t; }
	return result;
}

void Console::writeHex(uint64_t hex) {
	write("0x");
	write(itoa(hex, 16));
}
```
`writeHex(int)` i `itoa(int,int)` zostają nietknięte.

- [ ] **Step 4: Run + convcheck**

Run: `make test`
Expected: **GREEN** — oba CHECK przechodzą; reszta bez regresji; pokrycie ≥90%.

Run: `make convcheck FILE=drivers/Console.cpp`
Expected: brak ostrzeżeń `-Wconversion` dla `Console.*`.

- [ ] **Step 5: Commit**
```bash
git add drivers/Console.h drivers/Console.cpp tests/test_console.cpp
git commit -m "drivers: add 64-bit Console::writeHex/itoa overloads (print full addresses)"
```

---

## Task 6: Framebuffer — arytmetyka offsetu 64-bitowa (§4 #6)

`pixelAt`/`fbFillRect`/`fbScrollUp` liczą offset jako `(uint32_t) y * pitch` — przy dużym FB
(pitch * height) iloczyn przepełnia 32 bity, zanim doda się do `base`. Liczymy w `size_t`
(LP64: 64-bit) tak, by offset nie przepełniał.

**Files:**
- Modify: `drivers/Framebuffer.cpp`
- Modify: `tests/test_framebuffer.cpp` (duży pitch)

- [ ] **Step 1: Test — offset wiersza nie przepełnia przy dużym pitch (RED)**

Test pisze do logicznego wiersza, którego `y*pitch` przekracza 4 GiB, na *udawanym* surface (nie
alokujemy realnie 4 GiB — sprawdzamy obliczony offset przez `pixelAt`-równoważnik). Dodaj testowalny
hak: wystaw `static size_t fbByteOffset(const FbSurface&, uint32_t x, uint32_t y)` albo asercję na
`fbPutPixel` z bezpieczną areną i dużym `pitch` mieszczącym się w `size_t`:
```cpp
TEST_CASE("row offset uses 64-bit arithmetic (no 32-bit wrap at large pitch*y)") {
	// pitch chosen so that y*pitch crosses 2^32 for y within height; we only verify the
	// computed byte offset via fbByteOffset (pure, no real allocation of the surface).
	FbSurface s{ (uint8_t*) 0, /*pitch*/ 0x01000000u, /*w*/ 0x003FFFFFu, /*h*/ 0x200u, 32 };
	size_t off = kernel::fbByteOffset(s, 0, 0x101);   // 0x101 * 0x01000000 = 0x1_01000000 (>4 GiB)
	CHECK(off == (size_t) 0x101 * 0x01000000u);
	CHECK((off >> 32) != 0);                          // genuinely past 4 GiB
}
```
Run: `make test`
Expected: **RED** — brak `fbByteOffset` / obcięcie do 32 bit.

- [ ] **Step 2: Wprowadzić 64-bitową arytmetykę w `drivers/Framebuffer.cpp`**

Wystaw czysty, testowalny helper i użyj go (oraz `size_t`) w pętlach:
```cpp
// Byte offset of pixel (x,y) within the surface — 64-bit so a large pitch*y never wraps.
size_t fbByteOffset(const FbSurface& s, uint32_t x, uint32_t y) {
	return (size_t) y * s.pitch + (size_t) x * (s.bpp / 8);
}

static inline uint8_t* pixelAt(const FbSurface& s, uint32_t x, uint32_t y) {
	return s.base + fbByteOffset(s, x, y);
}
```
W `fbFillRect` (gałąź bpp==32) i `fbScrollUp` zamień `(uint32_t) yy * s.pitch` /
`(uint32_t) y * wordsPerRow` na `(size_t) yy * s.pitch` / `(size_t) y * wordsPerRow` (oraz
indeksy pętli pozostają `uint32_t` na współrzędne, ale offset bajtowy/słowny liczony w `size_t`).
Zadeklaruj `size_t fbByteOffset(const FbSurface&, uint32_t, uint32_t);` w `drivers/Framebuffer.h`.

- [ ] **Step 3: Run + convcheck**

Run: `make test`
Expected: **GREEN** — offset 64-bitowy; istniejące testy FB bez regresji.

Run: `make convcheck FILE=drivers/Framebuffer.cpp`
Expected: brak ostrzeżeń `-Wconversion` dla `Framebuffer.*`.

- [ ] **Step 4: Commit**
```bash
git add drivers/Framebuffer.cpp drivers/Framebuffer.h tests/test_framebuffer.cpp
git commit -m "drivers: Framebuffer row/pixel offsets use 64-bit (size_t) arithmetic"
```

---

## Task 7: String / List — `length`/`capacity`/`count` na `size_t` (§4 Istotne)

`String::length` i `List::capacity/capacityInc/count` są `int` — działa, ale niespójne na LP64 i
generuje ostrzeżenia `-Wconversion` (np. `int len = strlen(...)`, indeksy). Poszerzamy na `size_t`,
utrzymując semantykę. To pliki-liście dołączane wszędzie, więc muszą być czyste *przed* agregatami
(Kernel.cpp/Exec.cpp).

**Files:**
- Modify: `lib/String.h`, `lib/List.h`
- Istniejące: `tests/test_string.cpp`, `tests/test_list.cpp` (zostają zielone; w razie potrzeby
  dostosuj typy lokalnych zmiennych w testach)

- [ ] **Step 1: `lib/String.h` — `size_t length`**

Zamień pole i powiązane lokalne na `size_t`:
```cpp
class String {
	char* textArray;
	size_t length;
public:
	String(const char* text) {
		length = strlen(text);                 // strlen -> size_t (no narrowing)
		textArray = (char*) malloc(length + 1);
		memcpy(textArray, text, length);
		textArray[length] = 0;
	}
	...
	size_t getLenght() { return length; }      // keep the (historic) name; widen the type
	...
	int indexOf(String str, int start) {
		if ((size_t) start >= length) return -1;
		char* ptr = strstr(textArray + start, str.textArray);
		if (ptr == 0) return -1;
		return (int) (ptr - textArray);
	}
```
`append`: `size_t totalLenght = length + str.length;`. `operator[](int)` zostaje (indeks z `int`
jest świadomy; rzutuj jeśli `-Wconversion` zgłosi). `itoa(int,int)` w `String` zostaje bez zmian.

> Uwaga zgodności: `getLenght()` jest wołane w wielu miejscach jako `int`/w arytmetyce; zmiana
> zwrotu na `size_t` może rodzić zwężenia u woła­jących MI — te wyłapie `convcheck` ich własnych
> plików (głównie ext/Vfs porównują z `int i`). Tam, gdzie pojawi się ostrzeżenie, rzutuj u
> woła­jącego (np. `for (size_t i = 0; i < s.getLenght(); i++)`), nie cofaj typu w `String`.

- [ ] **Step 2: `lib/List.h` — `size_t capacity/capacityInc/count`**
```cpp
template<class T>
class List {
	T* array;
	size_t capacity;
	size_t capacityInc;
	size_t count;
public:
	List() : capacity(10), capacityInc(10), count(0) {
		array = (T*) malloc(sizeof(T) * capacity);
	}
	void insert(size_t index, T item) {
		if (count + 1 > capacity) increaseCapacity();
		if (index < count)
			memcpy(array + index + 1, array + index, (count - index) * sizeof(T));
		array[index] = item; count++;
	}
	void increaseCapacity() {
		capacity += capacityInc;
		array = (T*) realloc((void*) array, capacity * sizeof(T));
	}
	void removeAt(size_t index) {
		if (index + 1 < count) {
			memcpy(array + index, array + index + 1, (count - index - 1) * sizeof(T));
			count--; return;
		}
		if (index + 1 == count) count--;
	}
	size_t getCount() { return count; }
	T& operator[](size_t index) { return array[index]; }
	...
};
```
(Zwróć uwagę: `removeAt` poprawia też dotychczasowy off-by-one w `memcpy` rozmiarze —
`count - index - 1`, nie `count - index + 1`; zachowaj zgodność z istniejącymi testami.)

> Uwaga zgodności: woła­jący iterują `for (int i = 0; i < list.getCount(); i++)`. `getCount()` →
> `size_t` daje porównanie `int < size_t` (ostrzeżenie). Te wystąpienia w MI (ext/Vfs) poprawia
> ich własny convcheck w Tasku 8/9 — pętle przechodzą na `size_t i`.

- [ ] **Step 3: Run + convcheck**

Run: `make test`
Expected: **GREEN** — `test_string`/`test_list` (i wszystko, co używa String/List) bez regresji.

Run: `make convcheck FILE=lib/String.cpp` oraz `make convcheck FILE=lib/List.cpp`
Expected: brak ostrzeżeń `-Wconversion` dla `String.*` / `List.*`.

- [ ] **Step 4: Commit**
```bash
git add lib/String.h lib/List.h tests/test_string.cpp tests/test_list.cpp
git commit -m "lib: String/List sizes are size_t (LP64-clean), fix List::removeAt off-by-one"
```

---

## Task 8: BlockDevice `lba` → `uint64_t`; ext numery bloków (§4 Istotne)

`BlockDevice::readSectors(unsigned lba, …)` ma dwuznaczny `unsigned` — jawnie `uint64_t` (dyski >2 TiB
adresują sektory >4 mld). Implementacje MI (`RamBlockDevice`) i MD (`AtaBlockDevice`) dostosowane.
ext: struktury on-disk są stałoszerokościowe (`int` = 32-bit i na LP64, więc `memcpy` z dysku jest
poprawny — **bez zmian**), ale numer bloku ext4 jest 48-bitowy; dziś używamy tylko `startLo`
(limit 4 mld bloków = świadomy, udokumentowany pułap). Składamy `startHi|startLo`, gdzie tanie,
i utrzymujemy adresowanie cache 32-bitowe z jawną notą.

**Files:**
- Modify: `drivers/BlockDevice.h`
- Modify: `drivers/RamBlockDevice.{h,cpp}` (MI, testowane), `arch/x86/drivers/AtaBlockDevice.{h,cpp}`
  i `arch/x86_64/drivers/...` (MD; sygnatura — patrz Zależności)
- Modify: `fs/Ext4Filesystem.h` (komentarz 48-bit + złożenie `startHi`)
- Istniejące: `tests/test_ramblockdevice.cpp`, `tests/test_ext4.cpp` (zielone)

- [ ] **Step 1: Test — duże `lba` dociera nieobcięte do urządzenia (RED)**

`RamBlockDevice` jest pamięciowy i mały, ale możemy sprawdzić, że *sygnatura* przenosi 64-bitowe
`lba` bez zwężenia, używając atrapy, która zapamiętuje przekazane `lba`:
```cpp
TEST_CASE("BlockDevice::readSectors carries a 64-bit lba without truncation") {
	struct Spy : kernel::BlockDevice {
		uint64_t seen = 0;
		int readSectors(uint64_t lba, unsigned, void*) override { seen = lba; return 0; }
		int writeSectors(uint64_t, unsigned, const void*) override { return 0; }
		unsigned sectorSize() override { return 512; }
		const char* name() override { return "spy"; }
	} dev;
	dev.readSectors(0x1234ABCDEull, 1, nullptr);     // lba > 4 G sectors
	CHECK(dev.seen == 0x1234ABCDEull);
}
```
Run: `make test`
Expected: **RED** — `readSectors(unsigned, …)` obcina `0x1234ABCDE` do `0x234ABCDE`.

- [ ] **Step 2: Poszerzyć interfejs `drivers/BlockDevice.h`**
```cpp
	virtual int readSectors(uint64_t lba, unsigned count, void* buf) = 0;
	virtual int writeSectors(uint64_t lba, unsigned count, const void* buf) = 0;
```
(Dodaj `#include <stdint.h>`.)

- [ ] **Step 3: Dostosować implementacje**

`drivers/RamBlockDevice.{h,cpp}`: zmień sygnatury na `uint64_t lba`; wewnątrz offset bufora =
`lba * sectorSize()` w `size_t`/`uint64_t`. `arch/x86/drivers/AtaBlockDevice.{h,cpp}` (i jego
odpowiednik x86_64 z Planu 5): sygnatura `uint64_t lba`; ATA28/LBA28 ścieżka rzutuje świadomie do
28-bit z notą (dysk QEMU mieści się; LBA48 to przyszła praca). Te są MD/wyłączone z pokrycia, ale
muszą się kompilować, by kernel linkował.

- [ ] **Step 4: ext — udokumentować/złożyć 48-bit (Ext4Filesystem.h)**

W `Ext4Filesystem::resolveBlock` (leaf), gdzie liczymy fizyczny blok, złóż wysoką część (tania,
poprawia limit) i dodaj notę o pułapie cache 32-bit:
```cpp
		if (h->depth == 0) {
			Ext4Extent* ex = (Ext4Extent*) (cur + sizeof(Ext4ExtentHeader));
			for (int i = 0; i < h->entries; i++) {
				unsigned start = ex[i].fileBlock;
				unsigned end = start + ex[i].len;
				if (fileBlockIndex >= start && fileBlockIndex < end) {
					// ext4 physical block is 48-bit (startHi:startLo). We combine both, but the
					// block cache / resolveBlock return are still 32-bit block-addressed — a
					// conscious cap (~16 TiB at 4 KiB blocks) ample for our images. Documented limit.
					uint64_t phys = ((uint64_t) ex[i].startHi << 32) | (uint64_t) ex[i].startLo;
					return (unsigned) (phys + (fileBlockIndex - start));
				}
			}
			return 0;
		}
```
Struktury `Ext2*` (on-disk, `int`-owe) zostają — `memcpy` z dysku jest 32-bit-stały i poprawny na
LP64. Tam, gdzie convcheck ext zgłosi zwężenia z `String::getLenght()`/`List::getCount()` (Task 7),
zmień pętle woła­jące na `size_t i` (mechaniczne).

- [ ] **Step 5: Run + convcheck**

Run: `make test`
Expected: **GREEN** — Spy widzi pełne 64-bitowe `lba`; testy ext2/ext4/RamBlockDevice bez regresji.

Run: `make convcheck FILE=fs/ExtFilesystem.cpp` oraz `make convcheck FILE=drivers/RamBlockDevice.cpp`
Expected: brak ostrzeżeń `-Wconversion` wskazujących na te pliki (ext4 jest header-only — sprawdzany
tranzytywnie przez `ExtFilesystem.cpp`).

- [ ] **Step 6: Commit**
```bash
git add drivers/BlockDevice.h drivers/RamBlockDevice.h drivers/RamBlockDevice.cpp \
        arch/x86/drivers/AtaBlockDevice.h arch/x86/drivers/AtaBlockDevice.cpp \
        fs/Ext4Filesystem.h tests/test_ramblockdevice.cpp
git commit -m "drivers/fs: BlockDevice lba is uint64_t; ext4 composes 48-bit phys block (documented cap)"
```

---

## Task 9: Vfs::mmapInfo — `physOut` na `uint64_t*` (§4 Istotne)

`FileSystem::mmapInfo(String, unsigned*, unsigned*)` zwraca adres fizyczny przez `unsigned*` — adres
MMIO (framebuffer) może być >4 GiB. Poszerzamy `physOut` na `uint64_t*` przez interfejs
(`FileSystem`/`Vfs`/`Syscalls`) i jego implementacje (`SynthFs`, `Fb0Device`/`Fbdev`).

**Files:**
- Modify: `fs/Vfs.h` (virtual + `Vfs::mmapInfo`)
- Modify: `fs/Vfs.cpp`, `fs/SynthFs.{h,cpp}`, `drivers/Fbdev.{h,cpp}` / `drivers/Fb0Device.*`
- Modify: `kernel/Syscall.h` / `kernel/Syscall.cpp` (`Syscalls::mmapInfo`)
- Modify: `tests/test_syscall.cpp` / `tests/test_synthfs.cpp` (atrapa zwraca adres >4 GiB)

- [ ] **Step 1: Test — phys >4 GiB przechodzi przez mmapInfo nieobcięte (RED)**

W `tests/test_syscall.cpp` atrapa device-fs (obecnie `int mmapInfo(unsigned* p, unsigned* l)`) →
`uint64_t* p`:
```cpp
	int mmapInfo(String, uint64_t* p, unsigned* l) override { *p = 0x1FF000000ull; *l = 0x2000; return 0; }
...
	uint64_t p; unsigned l;
	CHECK(sc.mmapInfo(fd, &p, &l) == 0);
	CHECK(p == 0x1FF000000ull);          // > 4 GiB, not truncated to 0xFF000000
```
Run: `make test`
Expected: **RED** — sygnatury 32-bit obcinają `0x1FF000000` do `0xFF000000`.

- [ ] **Step 2: Poszerzyć interfejs i implementacje**

`fs/Vfs.h`:
```cpp
	virtual int mmapInfo(String, uint64_t*, unsigned*) { return -22; }   // -EINVAL
	...
	int mmapInfo(String path, uint64_t* physOut, unsigned* lenOut);      // Vfs
```
`fs/Vfs.cpp`: `int Vfs::mmapInfo(String path, uint64_t* physOut, unsigned* lenOut)` — przekaż dalej.
`fs/SynthFs.*` + char-device backend (`drivers/Fb0Device`/`Fbdev`): override `mmapInfo(String,
uint64_t*, unsigned*)`, wypełnij phys 64-bitowym adresem framebuffera.
`kernel/Syscall.h`: `int mmapInfo(int fd, uint64_t* physOut, unsigned* lenOut);`
`kernel/Syscall.cpp`: tak samo, przekazuje do `vfs->mmapInfo`.

- [ ] **Step 3: Dispatch — SYS_mmap2 (zmiana drobna, w Tasku 11 finalizowana)**

W `kernel/SyscallDispatch.cpp` (`SYS_mmap2`) zmienne `unsigned phys = 0` → `uint64_t phys = 0`;
`g_sys->mmapInfo(fd, &phys, &dlen)`. (Reszta `mmuMapUserFb` bierze phys — kontrakt `arch/mmu.h`
poszerzony w Planie 3; tu tylko zmienna lokalna i wywołanie. Pełny przegląd dispatchu w Tasku 11.)

- [ ] **Step 4: Run + convcheck**

Run: `make test`
Expected: **GREEN** — phys 64-bitowy; `test_synthfs`/`test_syscall` bez regresji.

Run: `make convcheck FILE=fs/Vfs.cpp` oraz `make convcheck FILE=fs/SynthFs.cpp`
Expected: brak ostrzeżeń `-Wconversion` dla `Vfs.*` / `SynthFs.*`.

- [ ] **Step 5: Commit**
```bash
git add fs/Vfs.h fs/Vfs.cpp fs/SynthFs.h fs/SynthFs.cpp drivers/Fbdev.h drivers/Fbdev.cpp \
        drivers/Fb0Device.h drivers/Fb0Device.cpp kernel/Syscall.h kernel/Syscall.cpp \
        kernel/SyscallDispatch.cpp tests/test_syscall.cpp
git commit -m "fs: Vfs/Syscalls mmapInfo returns a 64-bit physical address (uint64_t*)"
```

---

## Task 10: Syscall.h — `Fd`/`lseek`/`LinuxStat` na 64-bit `off_t` (§4 #8, #9)

`struct Fd { unsigned offset; unsigned size; }` i `lseek(int,int,…)` ograniczają pliki do 4 GiB;
`struct LinuxStat::st_size`/`st_ino` są `unsigned`. Na x86_64 ABI Linuksa `off_t`/`st_size`/`st_ino`
są 64-bitowe, a picolibc x86_64 oczekuje innego layoutu `struct stat`. Wprowadzamy
`typedef int64_t off_t` (64-bit ze znakiem), poszerzamy `Fd`/`lseek` i przerysowujemy `LinuxStat`.

**Files:**
- Modify: `kernel/Syscall.h` (`off_t`, `Fd`, `lseek`, `truncate/ftruncate`, `LinuxStat`)
- Modify: `kernel/Syscall.cpp` (`lseek`, `fillStat`, ścieżki offsetu)
- Modify: `tests/test_syscall.cpp` (lseek >4 GiB; fstat 64-bit size)

- [ ] **Step 1: Test — lseek ponad 4 GiB nie obcina offsetu (RED)**
```cpp
TEST_CASE("lseek past 4 GiB keeps the full 64-bit offset") {
	Syscalls sc(mountFixture(), sink);
	int fd = sc.open(String("/hello.txt"), 0);    // any regular file in the fixture
	REQUIRE(fd >= 3);
	off_t pos = sc.lseek(fd, (off_t) 0x1_0000_0000ll, SEEK_SET);   // 4 GiB
	CHECK(pos == (off_t) 0x100000000ll);          // not truncated to 0
}
```
(Jeśli fixture nie ma `/hello.txt`, użyj istniejącej ścieżki z `mountFixture()` — patrz inne testy.)

Run: `make test`
Expected: **RED** — `lseek(int,int,…)` + `Fd.offset` (32-bit) obcinają 4 GiB do 0.

- [ ] **Step 2: `kernel/Syscall.h` — `off_t`, `Fd`, sygnatury**
```cpp
typedef long long off_t;     // 64-bit signed file offset (x86_64 ABI)
...
	struct Fd {
		bool used;
		bool isConsole;
		String path;
		off_t offset;        // 64-bit file position
		off_t size;          // 64-bit cached size
		...
	};
...
	int read(int fd, void* buf, unsigned n);
	int write(int fd, const void* buf, unsigned n);
	off_t lseek(int fd, off_t off, int whence);
	...
	int truncate(String path, off_t length);
	int ftruncate(int fd, off_t length);
```

`LinuxStat` — przerysowany pod x86_64 (64-bitowe `st_ino`/`st_size`, 64-bit czas):
```cpp
// Kernel-internal stat buffer filled by stat/fstat and marshalled to userland by the libc-glue.
// Field WIDTHS match the x86_64 ABI: 64-bit st_ino/st_size/time. The exact on-wire field ORDER is
// owned by the userland stat marshaller (user/libc-glue) — coordinate any reorder with Plan 6/8.
struct LinuxStat {
	uint64_t st_ino;     // 64-bit inode number
	uint32_t st_mode;    // S_IF* | perms
	uint32_t st_nlink;
	uint32_t st_uid;
	uint32_t st_gid;
	uint64_t st_size;    // 64-bit off_t
	uint64_t st_blocks;  // 512-byte block count
	int64_t  st_mtime;   // 64-bit time_t
};
```

- [ ] **Step 3: `kernel/Syscall.cpp` — lseek + fillStat + offsety**
```cpp
off_t Syscalls::lseek(int fd, off_t off, int whence) {
	if (!valid(fd)) return -EBADF;
	off_t base;
	if (whence == SEEK_SET)      base = 0;
	else if (whence == SEEK_CUR) base = fds[fd].offset;
	else if (whence == SEEK_END) base = fds[fd].size;
	else return -EINVAL;
	off_t pos = base + off;
	if (pos < 0) return -EINVAL;
	fds[fd].offset = pos;
	return pos;
}
```
W `read`/`write`/`getdents64`: `fds[fd].offset += (off_t) r;` (zamiast `(unsigned) r`); kursor
`getdents64` używa `off_t i` zamiast `int i` (lub zachowaj `int` z jawnym rzutowaniem — kursor
katalogu mieści się w 32-bit, ale typ pola to teraz `off_t`). `fillStat`:
```cpp
	out->st_size = (uint64_t) st.size;
	out->st_ino  = st.ino ? (uint64_t) st.ino : 1;
	out->st_blocks = ((uint64_t) st.size + 511) / 512;
	out->st_mtime  = (int64_t) st.mtime;
```
`truncate/ftruncate` przyjmują `off_t length` (rzutuj do `vfs->truncate(... , (unsigned) length)`
póki `FileStat.size`/Vfs są `unsigned` — nota: pełne 64-bit rozmiary plików w ext to osobny dług
funkcjonalny poza tym planem; tu domykamy *offset* API i layout stat).

- [ ] **Step 4: Run + convcheck**

Run: `make test`
Expected: **GREEN** — lseek 4 GiB zwraca pełny offset; `test_syscall` (lseek/fstat) bez regresji.

Run: `make convcheck FILE=kernel/Syscall.cpp`
Expected: brak ostrzeżeń `-Wconversion` dla `Syscall.*`.

- [ ] **Step 5: Commit**
```bash
git add kernel/Syscall.h kernel/Syscall.cpp tests/test_syscall.cpp
git commit -m "kernel: 64-bit off_t for Fd/lseek/truncate; LinuxStat widened to the x86_64 layout"
```

---

## Task 11: kernelSyscall — args `uintptr_t`, zwrot `long` (§4 #3, centralny punkt ABI)

To centralny punkt ABI: dziś `kernelSyscall(int nr, unsigned a0..a5, …)` obcina 64-bitowe wskaźniki i
rozmiary, a zwrot `int` obcina 64-bitowy adres `mmap`. Finalna sygnatura (SHARED CONTRACT):
`long kernelSyscall(long nr, uintptr_t a0..a5, arch::TrapFrame*)`. Wnętrze dispatchu: `long ret`,
adresy z `uintptr_t`, a `mmap2/brk/munmap` zwracają pełny adres. Liczne wywołania `Syscalls::*`
(które przyjmują węższe typy: `int fd`, `unsigned n`) dostają jawne, świadome rzutowania — to praca
mechaniczna, którą wylicza kompilator pod `-Wconversion`.

**Files:**
- Modify: `arch/include/arch/syscall.h` (deklaracja)
- Modify: `kernel/SyscallDispatch.cpp` (definicja + casty)
- Modify: `arch/x86/cpu/syscall_x86.cpp` (i686 caller — implicit-widen args; zwrot truncuje do
  32-bit `eax` jak dziś) i `arch/x86_64/cpu/syscall_x86_64.cpp` (Plan 6 — przekazuje pełne 64-bit)
- Modify: `tests/test_socketsys.cpp` (round-trip 64-bitowego wskaźnika przez dispatch)

- [ ] **Step 1: Test — wskaźnik 64-bitowy przeżywa dispatch (RED)**

`tests/test_socketsys.cpp` (lub nowy `tests/test_dispatch_abi.cpp`) wywołuje `kernel::kernelSyscall`
bezpośrednio, hostowo (LP64), z adresem >4 GiB i sprawdza, że nie został obcięty. Najprościej przez
`SYS_write` na fd konsoli z buforem na sztucznie wysokim adresie nie jest bezpieczne (deref). Zamiast
tego użyj syscalla, który tylko *waliduje*/zwraca: np. `SYS_mmap2` z `length==0` (zwraca -EINVAL bez
deref), albo dodaj asercję typów przez `static_assert` na sygnaturze:
```cpp
TEST_CASE("kernelSyscall ABI is LP64-wide (args uintptr_t, result long)") {
	using Fn = long(*)(long, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
	                   uintptr_t, arch::TrapFrame*);
	Fn f = &kernel::kernelSyscall;        // compiles only if the signature matches exactly
	(void) f;
	CHECK(sizeof(uintptr_t) == sizeof(void*));   // sanity on the host (LP64)
}
```
Run: `make test`
Expected: **RED** — przypisanie `&kernelSyscall` do `Fn` nie kompiluje (stara sygnatura
`int(int,unsigned,…)`).

- [ ] **Step 2: `arch/include/arch/syscall.h` — finalna deklaracja**
```cpp
namespace kernel {
// MI dispatch: nr in rax, up to 6 args in rdi/rsi/rdx/r10/r8/r9 (x86_64) or ebx/.../ebp (i686).
// Args are uintptr_t so 64-bit user pointers/sizes pass intact; the result is `long` so a 64-bit
// address (mmap) returns whole. `tf` is the opaque trap frame (execve/fork rewrite it).
long kernelSyscall(long nr, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3,
		uintptr_t a4, uintptr_t a5, arch::TrapFrame* tf);
}
```
(Dodaj `#include <stdint.h>`.)

- [ ] **Step 3: `kernel/SyscallDispatch.cpp` — definicja + casty**

Zmień nagłówek funkcji i `int ret` → `long ret`:
```cpp
long kernelSyscall(long nr, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3,
		uintptr_t a4, uintptr_t a5, arch::TrapFrame* tf) {
	long ret = -38;   // -ENOSYS
	Syscalls* g_sys = ProcTable::current()->sys;
	switch (nr) {
```
Wzorce rzutowań (mechaniczne — kompilator wylicza wszystkie pod `-Wconversion`):
- fd / małe inty: `(int) a0`, np. `g_sys->read((int) a0, (void*) a1, (unsigned) a2);`
- rozmiary: `(unsigned) a2`;
- wskaźniki: `(void*) a1`, `(LinuxStat*) a2`, `(const char*) a0` — z `uintptr_t` czysto;
- zwroty adresów: `SYS_mmap2`/`SYS_brk`/`SYS_munmap` ustawiają `ret = (long) va;` (pełny adres),
  a `mmuMmapBase()/mmuMmapMax()` itd. zwracają `uintptr_t` (kontrakt z Planu 3);
- `futexSyscall(uaddr, …)`/`socketOp`/`doSelect` — przyjmują `uintptr_t`/wskaźniki zamiast `unsigned`
  (zmień ich sygnatury lokalnie na `uintptr_t` tam, gdzie reprezentują adresy: `iovGather(const
  uintptr_t* iov, …)`, `A[6]` jako `uintptr_t`, `futexSyscall(uintptr_t uaddr, …)`).

Pomocnicze `static` w tym pliku, które dziś biorą `const unsigned*`/`unsigned` jako adresy
(`iovGather`, `iovScatter`, `socketOp`, `doSelect`, `futexSyscall`), zmień na `uintptr_t`/
`const uintptr_t*` analogicznie — to one rozpakowują `A[]`. `A[6]` deklaruj jako
`uintptr_t A[6]`.

- [ ] **Step 4: i686 caller (`arch/x86/cpu/syscall_x86.cpp`) — dopasować**

Wywołanie już przekazuje `r->eax, r->ebx, …` (32-bit `unsigned`) — implicit widen do `uintptr_t` jest
bezpieczny. Zwrot: `r->eax = (unsigned) kernel::kernelSyscall(...)` — `long`→`unsigned` świadome
obcięcie do 32-bitowego `eax` (i686 ma tylko 32-bit rejestry); zostaw jak jest. (x86_64 caller z
Planu 6 wpisze pełny `rax`.) i686 nie jest objęty bramką `KWFLAGS`, więc nie wymaga castów.

- [ ] **Step 5: Run + convcheck**

Run: `make test`
Expected: **GREEN** — `static_assert`/przypisanie `Fn` kompiluje; cały `test_syscall`/`test_socketsys`
bez regresji; pokrycie ≥90%.

Run: `make convcheck FILE=kernel/SyscallDispatch.cpp`
Expected: brak ostrzeżeń `-Wconversion` dla `SyscallDispatch.*`.

- [ ] **Step 6: Commit**
```bash
git add arch/include/arch/syscall.h kernel/SyscallDispatch.cpp arch/x86/cpu/syscall_x86.cpp \
        tests/test_socketsys.cpp
git commit -m "kernel: kernelSyscall takes uintptr_t args and returns long (LP64 syscall ABI)"
```

---

## Task 12: Exec.cpp — `STAGE_*` z `arch/mmu.h`, `entry`/`esp` na `uintptr_t` (§4 #7)

`STAGE_BASE`/`STAGE_CAP` są lokalnymi literałami `unsigned`, a `entry`/`esp` to `unsigned` — na 64-bit
adres staging/entry/SP gubi górne bity. Konsumujemy okna VA z kontraktu `arch/mmu.h`
(`arch::mmuStageBase()`/`mmuStageCap()`, dodane w Planie 3) i poszerzamy adresy na `uintptr_t`.

**Files:**
- Modify: `kernel/Exec.cpp`
- (Pokrycie ścieżki: `tests/test_dynloader.cpp` / `test_nxeloader.cpp` — zostają zielone)

- [ ] **Step 1: Zdjąć lokalne literały, konsumować kontrakt**

Zamień:
```cpp
static const unsigned STAGE_BASE = 0x800000;
static const unsigned STAGE_CAP  = 0x800000;
...
static int loadStaged(unsigned* entryOut) {
	char* image = (char*) STAGE_BASE;
	return NxeLoader::loadImage(image, STAGE_CAP, 0, 0, entryOut);
}
```
na (okna z `arch/mmu.h`; `uintptr_t`):
```cpp
// The staging window comes from the arch MMU contract (Plan 3 moved the VA-window literals into
// <arch/mmu.h>); MI no longer hard-codes 0x800000. mmuStageCap() == the per-process user window size.
static inline uintptr_t stageBase() { return arch::mmuStageBase(); }
static inline uintptr_t stageCap()  { return arch::mmuStageCap(); }

static int loadStaged(uintptr_t* entryOut) {
	char* image = (char*) stageBase();
	return NxeLoader::loadImage(image, stageCap(), 0, 0, entryOut);   // delta 0, no imports
}
```
W `execProgram`/`execve`: `char* image = (char*) stageBase();`, `if (st.size > stageCap()) …`,
`uintptr_t entry = 0;`, `uintptr_t esp = arch::archLoadUser(...)` (archLoadUser zwraca `uintptr_t` —
kontrakt z Planu 3/Plan 4). `NxeLoader::loadImage` `entryOut` jest `uintptr_t*` (poszerzone w
Planie 6 wraz z `.nxe` v4 — patrz Zależności).

- [ ] **Step 2: Run + convcheck**

Run: `make test`
Expected: **GREEN** — `test_dynloader`/`test_nxeloader` bez regresji (NxeLoader host-testowany).

Run: `make convcheck FILE=kernel/Exec.cpp`
Expected: brak ostrzeżeń `-Wconversion` dla `Exec.*` (przy obecności poszerzonych kontraktów
`arch/mmu.h`/`arch/usermode.h` z Planów 3/4; patrz Zależności).

- [ ] **Step 3: Commit**
```bash
git add kernel/Exec.cpp
git commit -m "kernel: Exec consumes stage window from <arch/mmu.h>; entry/esp are uintptr_t"
```

---

## Task 13: Kernel.cpp — rzutowania MMIO/BAR/FB + `freeCount`/`top` (§4 #4)

`Kernel.cpp` obcina adresy: `markRangeFree((uint32_t)base,(uint32_t)len)`,
`mmuMapKernelMmio((uint32_t)fb->addr,…)`, `FbInfo{(uint32_t)fbdev->addr,…}`,
`writeHex((int)nic.bar[0].addr)`. Po Taskach 2/5 i poszerzeniu kontraktu `arch/mmu.h` (Plan 3)
zdejmujemy te `(uint32_t)`/`(int)`-y i domykamy zwężenia z `freeCount()`/`bootMemTop()`.

**Files:**
- Modify: `kernel/Kernel.cpp`
- (Weryfikacja runtime: boot x86_64 — patrz Zależności / Step 4)

- [ ] **Step 1: Zdjąć obcinające rzutowania**
```cpp
// markFree: FrameAllocator now takes uint64_t — pass base/len straight through.
static void markFree(void* fa, uint64_t base, uint64_t len) {
	((FrameAllocator*) fa)->markRangeFree(base, len);
}
...
// initPaging: top is 64-bit. (arch::bootMemTop()/mmuInitKernel widened by Plans 2/3.)
uint64_t top = arch::bootMemTop();
g_frames.init(top);
arch::bootMemForEachUsable(&g_frames, markFree);
arch::mmuInitKernel(g_frames, top);
...
if (fb) {
	arch::mmuMapKernelMmio(fb->addr, (uint64_t) fb->pitch * fb->height);   // no (uint32_t) truncation
	arch::consoleActivateFramebuffer();
}
...
FbInfo info = { fb_addr_field /* uint64_t */, fbdev->pitch, fbdev->width, fbdev->height, fbdev->bpp };
```
Dla `FbInfo`: jeśli pole adresu w `FbInfo` (drivers/Fbdev.h) jest `uint32_t`, poszerz je na
`uint64_t` (framebuffer LFB nad RAM może siedzieć wysoko) i zaktualizuj konsumentów. BAR:
```cpp
Console::write(" BAR0=");
Console::writeHex(nic.bar[0].addr);     // 64-bit overload (Task 5); no (int) cast
```

- [ ] **Step 2: Domknąć zwężenia `/proc/meminfo`**
```cpp
unsigned sysMemTotalKb() { return (unsigned) (arch::bootMemTop() / 1024u); }
unsigned sysMemFreeKb()  { return (unsigned) (g_frames.freeCount() * (FRAME_SIZE / 1024u)); }
unsigned sysHeapTotalKb() { return (unsigned) (heapTotalBytes() / 1024u); }   // heap*Bytes now size_t
unsigned sysHeapFreeKb()  { return (unsigned) (heapFreeBytes() / 1024u); }
```
(`freeCount()` → `uint64_t`, `heap*Bytes()` → `size_t`; jawny `(unsigned)` na kB jest świadomy —
liczba kilobajtów RAM mieści się w 32-bit dla naszych targetów, z notą.)

- [ ] **Step 3: `make check-arch` + host build**

Run: `make check-arch`
Expected: `OK: MI layer is arch-clean.` (Kernel.cpp nie wprowadził żadnej wewnętrzności x86.)

Run: `make convcheck FILE=kernel/Kernel.cpp`
Expected: brak ostrzeżeń `-Wconversion` dla `Kernel.*` (przy poszerzonych kontraktach z Planów 2/3).

- [ ] **Step 4: (Runtime, gdy x86_64 bootuje — patrz Zależności) sanity boot**

Gdy Plany 2–5 dają bootujący kernel x86_64: zbuduj obraz i potwierdź, że boot-splash, `/proc/meminfo`
(`MemTotal`/`MemFree`) i `BAR0=` w `pciScanReport` wypisują sensowne, pełne wartości (BAR-y i FB nad
4 GiB nieobcięte). Wzorzec headless-QEMU z CLAUDE.md (screendump → PNG).

- [ ] **Step 5: Commit**
```bash
git add kernel/Kernel.cpp drivers/Fbdev.h
git commit -m "kernel: drop 32-bit address truncation in Kernel (MMIO/BAR/FB, frame map, meminfo)"
```

---

## Task 14: Włączyć bramkę `-Wconversion` na buildzie x86_64 + domknięcie

Po przejściu wszystkich plików MI zapalamy bramkę: `KWFLAGS=-Wconversion -Werror=conversion` w
`arch/x86_64/arch.mk`, dzięki czemu każde przyszłe obcięcie LP64 łamie build x86_64. i686 zostaje bez
bramki (zielony do cut-over, §1.1). Dowodzimy czystości całego MI.

**Files:**
- Modify: `arch/x86_64/arch.mk` (`KWFLAGS`)
- Modify: `docs/superpowers/specs/2026-06-15-x86_64-migration-analysis.md` (odhaczenie kamienia 7)

- [ ] **Step 1: Zapalić bramkę w `arch/x86_64/arch.mk`**

Zamień (z Tasku 1):
```makefile
KWFLAGS ?=
```
na:
```makefile
# Plan 7 complete: the MI sweep is clean, so make any future LP64 truncation a hard error on the
# x86_64 build. i686 stays ungated (kept green only until cut-over, spec §1.1).
KWFLAGS ?= -Wconversion -Werror=conversion
```

- [ ] **Step 2: Pełny sweep MI pod bramką**

Dla każdego pliku MI źródłowego (lista = `MI_SOURCES`, katalogi `init kernel mm fs lib drivers net`):
```bash
for f in mm/FrameAllocator.cpp mm/Heap.cpp mm/memory_manager.cpp drivers/Console.cpp \
         drivers/Framebuffer.cpp lib/String.cpp lib/List.cpp drivers/RamBlockDevice.cpp \
         fs/Vfs.cpp fs/SynthFs.cpp fs/ExtFilesystem.cpp kernel/Syscall.cpp \
         kernel/SyscallDispatch.cpp kernel/Exec.cpp kernel/Kernel.cpp; do
  echo "== $f =="; make convcheck FILE="$f" || exit 1;
done
```
Expected: każdy plik kompiluje się **bez** ostrzeżeń `-Wconversion` (z `-Werror=conversion` przez
`KWFLAGS` w `CXXFLAGS` — `_convcheck` używa `CXXFLAGS`, więc `-Werror=conversion` jest aktywne).
Jeśli któryś zgłosi resztkowe zwężenie, dopisz świadome rzutowanie z notą i ujmij je w commicie
właściwego Tasku (lub tu, jako domknięcie).

- [ ] **Step 3: Pełny build x86_64 (gdy Plany 2–6 dostarczyły `arch/x86_64`)**

Run: `docker run --rm -v "$(pwd)":/src -w /src nanos-build make ARCH=x86_64 _all`
Expected: kernel x86_64 linkuje się **czysto pod `-Wconversion -Werror=conversion`**. (Jeśli Plany 2–6
jeszcze nie zlinkowały całego `arch/x86_64`, ten krok czeka na nie — patrz Zależności; do tego czasu
dowodem jest pełny per-plikowy sweep ze Step 2.)

- [ ] **Step 4: i686 bez regresji + host suite**

Run: `make build && make check-arch && make test`
Expected: i686 buduje się (KWFLAGS i686 puste); `OK: MI layer is arch-clean.`; host suite zielony,
pokrycie ≥90%.

- [ ] **Step 5: Odhaczyć kamień 7 w specu**

W `docs/superpowers/specs/2026-06-15-x86_64-migration-analysis.md`, §7, przy kamieniu 7 dopisać
„✅ zrealizowane w plans/2026-06-15-x86_64-plan-7-mi-lp64-cleanup.md".

- [ ] **Step 6: Commit**
```bash
git add arch/x86_64/arch.mk docs/superpowers/specs/2026-06-15-x86_64-migration-analysis.md
git commit -m "build: enable -Wconversion gate on the x86_64 kernel build (MI LP64 sweep complete)"
```

---

## Self-Review

- **Spec coverage:** Plan realizuje §4 w całości — Krytyczne #1 (FrameAllocator, T2), #2
  (memory_manager + Heap, T3–T4), #3 (kernelSyscall, T11), #4 (Kernel.cpp, T13), #5 (Console, T5),
  #6 (Framebuffer, T6), #7 (Exec.cpp, T12), #8 (Fd/lseek, T10), #9 (LinuxStat, T10); Istotne:
  String/List (T7), Heap (T3), Vfs::mmapInfo (T9), BlockDevice lba (T8), ext in-memory / ext4 48-bit
  (T8). Kamień 7 z §7 odhaczony w T14.
- **Latentne błędy → testy >4 GiB:** FrameAllocator alloc @5 GiB (T2), Heap/calloc przepełnienie
  `size_t` (T3/T4), Console hex 64-bit (T5), Framebuffer offset >4 GiB (T6), BlockDevice lba 64-bit
  (T8), lseek 4 GiB (T10), kernelSyscall sygnatura LP64 (T11) — każdy *czerwony* na wąskim typie.
- **`struct stat`:** LinuxStat przerysowany na 64-bitowe `st_ino`/`st_size`/czas; nota o koordynacji
  KOLEJNOŚCI pól z marshallerem userland (`user/libc-glue`, Plan 6/8) — szerokości są tu, layout
  on-wire tam.
- **Jedna finalna sygnatura:** `kernelSyscall(long, uintptr_t…, long)` współdzielona przez
  `<arch/syscall.h>` + dispatch + obie ścieżki `syscall_*`. Edycje addytywne wobec MD callerów
  (i686 implicit-widen + istniejący `(unsigned)`-zwrot; x86_64 z Planu 6 wpisuje pełny `rax`).
- **MI-only / check-arch:** żadne zadanie nie wprowadza nagłówków ani inline-asm x86; `make
  check-arch` weryfikowany w T1/T13. Główny poligon = `nanos-test` (LP64), QEMU tylko jako sanity
  (T13 Step 4) gdy x86_64 bootuje.
- **Placeholders:** brak — każdy krok ma realny przed/po i komendę z oczekiwanym wynikiem.

## Zależności

- **Interleaving z Planami 2–6 (spec §7 kamień 7 — „równolegle"):** Plan 7 może lądować
  przyrostowo. Zadania **czysto MI** (T1–T11) nie wymagają `arch/x86_64` — `convcheck` kompiluje
  pojedynczy plik MI tylko wobec kontraktów `arch/include`, więc działają **dziś**, na obecnym
  drzewie, równolegle do Planu 2+.
- **T9 Step 3 / T11 / T12 / T13** zakładają poszerzone kontrakty `arch/mmu.h`/`arch/usermode.h`
  (`mmuStageBase`/`mmuStageCap`, `mmuMmapBase…→uintptr_t`, `mmuMapKernelMmio(uint64_t)`,
  `archLoadUser→uintptr_t`) z **Planu 3/4** oraz `NxeLoader::loadImage(entryOut=uintptr_t*)` z
  **Planu 6**. Jeśli te jeszcze nie wylądowały, T12/T13 czekają (a `convcheck` tych plików zgłosi
  zwężenie na granicy kontraktu — to sygnał, że kontrakt MD nie jest jeszcze poszerzony, nie błąd MI).
- **T14 Step 3** (pełny link `make ARCH=x86_64 _all` pod `-Werror=conversion`) wymaga, by Plany 2–6
  dostarczyły kompletne `arch/x86_64`. Do tego czasu dowodem czystości jest pełny per-plikowy sweep
  (T14 Step 2). i686 pozostaje zielony przez cały okres (KWFLAGS i686 puste).
- **Userland marshalling (LinuxStat layout):** koordynować z `user/libc-glue` (Plan 6/8) — Plan 7
  ustala szerokości pól; kolejność on-wire i wrappery `stat`/`fstat`/`lseek` (`off_t`) domyka Plan 6.

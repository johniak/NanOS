# x86_64 Plan 2 — Pełny boot + konsola + MI kmain

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Doprowadzić trampolinę long-mode z Planu 1 od „wpisuje literał na 0xB8000" do
stanu, w którym 64-bitowe `kentry64` woła **maszynowo-niezależne (MI) `kmain()`**, a ono
uruchamia **OKROJONY, minimalny `Kernel::start`** drukujący przez prawdziwą warstwę
formatowania `Console` (`<arch/console.h>` sink = `console_x86_64.cpp`), parsuje informacje
Multiboot1 (mapa pamięci + framebuffer) w 64-bicie (`bootinfo_x86_64.cpp`) i dobija do pętli
idle. **BEZ** storage stacku, BEZ schedulera, BEZ syscalli — to Plany 3–6. Build jest
**staged**: linkujemy tylko ten podzbiór `MI_SOURCES`, który jest potrzebny (Console +
memory_manager + bootinfo), nie cały kernel.

**Architecture:** Migracja-zastąpienie (patrz `docs/superpowers/specs/2026-06-15-x86_64-migration-analysis.md`,
§2.1 boot, §2.4 console_x86, §7 kamień 2). Zachowujemy przełącznik `ARCH` i podział MI/MD.
Plan 2 jest pierwszym, który linkuje obiekty MI dla x86_64 — dlatego wprowadza **osobny
katalog obiektów `bin/stage64/`**: obiekty MI mają wspólne basename'y z buildem i686
(`Console.o`, `memory_manager.o`…), a x86_64 produkuje je jako **elf64**, więc nie mogą leżeć
w tym samym `bin/` co elf32 i686 (zatrute formaty + niespójność mtime przy przełączaniu
`ARCH`). Staged kernel nie potrzebuje dysku, więc bootujemy **rescue-ISO GRUB** z Planu 1
(`-cdrom bin/nanos64.iso`), nie obraz dysku.

**Tech Stack:** Debian trixie cross-toolchain `x86_64-elf` (binutils 2.43 + gcc 14.2.0),
NASM (`-f elf64`), GNU ld, GRUB (`grub-mkrescue`, multiboot1), QEMU (`qemu-system-x86_64
-cpu qemu64`), Docker (`nanos-build`). Codegen kernela: `-mno-red-zone -mno-sse -mno-mmx
-mno-80387` (`KARCHFLAGS` z `arch/x86_64/arch.mk`).

**Reference / decyzje:** LP64-sweep całego MI (§4 specu) należy do **Planu 7**. Plan 2 robi
tylko **minimalny** podzbiór konieczny do zlinkowania i uruchomienia staged kernela: jedyna
realna poprawka to przeciążenie `Console::writeHex(unsigned long)` (drukowanie 64-bitowego
adresu — `writeHex(int)` obcina). Alokator `memory_manager` rzutuje `size` na `(unsigned)`,
co jest poprawne dla małych alokacji staged heapu — **pełny sweep odłożony do Planu 7, tu nie
dotykany.** Konstruktory globalne NIE są uruchamiane (loader woła kmain wprost); `.bss` jest
zerowane przez loader multiboot — wzorce inicjalizacji mirrorują i686 (statyki trywialne w
`.bss`, `heapInit` wołane ręcznie).

---

## File Structure

| Plik | Odpowiedzialność | Akcja |
|---|---|---|
| `arch/x86_64/boot/MultibootInfo.h` | POD Multiboot1 info + mmap entry + framebuffer (kopia x86; format on-disk jest staloszerokościowy, arch-neutralny) | Create |
| `arch/x86_64/boot/MultibootMmap.h` | deklaracje czystych helperów mapy pamięci (kopia x86) | Create |
| `arch/x86_64/boot/MultibootMmap.cpp` | parsowanie mapy + najwyższy używalny adres (kopia x86; już LP64-clean — `uintptr_t`) | Create |
| `arch/x86_64/boot/bootinfo_x86_64.cpp` | impl `<arch/bootinfo.h>` (`bootMemTop`/`bootMemForEachUsable`/`bootFramebuffer`); przejmuje wskaźnik Multiboot przez `bootSetMultibootInfo` | Create |
| `arch/x86_64/drivers/console_x86_64.cpp` | sink VGA tekstowy (80x25 @ 0xB8000), kursor przez porty 0x3D4/0x3D5, samodzielny (bez FbConsole) — impl `<arch/console.h>` | Create |
| `arch/x86_64/boot/bringup_stubs64.cpp` | shimy `<arch/cpu.h>` (`cpuDisableInterrupts`/`cpuHalt`) potrzebne do zlinkowania `memory_manager`; TYLKO staged, zastąpione przez `cpu_x86_64.cpp` w Planie 4 | Create |
| `arch/x86_64/boot/KernelStage64.cpp` | OKROJONY `kernel::Kernel::start` dla bring-upu (banner przez Console + bootinfo + heap, potem idle) | Create |
| `arch/x86_64/boot/entry64.cpp` | `kentry64`: stash wskaźnika Multiboot → `bootSetMultibootInfo`, wołanie MI `kmain()` (zastępuje proof-of-life z Planu 1) | Modify |
| `drivers/Console.h` | dodać przeciążenie `writeHex(unsigned long)` (minimalna poprawka LP64) | Modify |
| `drivers/Console.cpp` | impl `writeHex(unsigned long)` (szerokość adresu zależna od arch) | Modify |
| `Makefile` | osobny katalog obiektów `bin/stage64/`, zestaw `STAGE64_OBJS`, reguły staged, przepisany `_bringup64` + cel `_stage64`; aktualizacja echo w host-owym `bringup64` | Modify |

---

## Task 1: x86_64 nagłówki Multiboot (info + mmap)

Staged `bootinfo_x86_64` potrzebuje tych samych POD-ów i czystych helperów co i686. Format
on-disk Multiboot1 jest staloszerokościowy (32-bit pola), więc to wierna kopia z `arch/x86`,
osadzona w `arch/x86_64/boot/` (VPATH dla `ARCH=x86_64` nie obejmuje `arch/x86`).

**Files:**
- Create: `arch/x86_64/boot/MultibootInfo.h`
- Create: `arch/x86_64/boot/MultibootMmap.h`
- Create: `arch/x86_64/boot/MultibootMmap.cpp`

- [ ] **Step 1: Utworzyć `arch/x86_64/boot/MultibootInfo.h`**

```cpp
/*
 * MultibootInfo.h (x86_64) — POD layout of the Multiboot 1 information structure and a
 * memory-map entry, as passed by GRUB. The on-disk Multiboot fields are fixed-width
 * (32-bit), so this is a verbatim mirror of arch/x86/boot/MultibootInfo.h; the long-mode
 * port keeps its own copy because the x86_64 VPATH does not include arch/x86.
 *
 * Reference offsets (Multiboot 1): flags@0, mem_lower@4, mem_upper@8,
 * mmap_length@44, mmap_addr@48.
 */
#pragma once
#include <stdint.h>

namespace kernel {

struct MultibootInfo {
	uint32_t flags;            // +0
	uint32_t mem_lower;        // +4   KB below 1MB
	uint32_t mem_upper;        // +8   KB above 1MB
	uint32_t boot_device;      // +12
	uint32_t cmdline;          // +16
	uint32_t mods_count;       // +20
	uint32_t mods_addr;        // +24
	uint32_t syms[4];          // +28..+43
	uint32_t mmap_length;      // +44  bytes
	uint32_t mmap_addr;        // +48  phys addr of the first MmapEntry
	uint32_t drives_length;    // +52
	uint32_t drives_addr;      // +56
	uint32_t config_table;     // +60
	uint32_t boot_loader_name; // +64
	uint32_t apm_table;        // +68
	uint32_t vbe_control_info; // +72
	uint32_t vbe_mode_info;    // +76
	uint16_t vbe_mode;         // +80
	uint16_t vbe_interface_seg;// +82
	uint16_t vbe_interface_off;// +84
	uint16_t vbe_interface_len;// +86
	uint64_t framebuffer_addr; // +88   linear framebuffer physical address
	uint32_t framebuffer_pitch;// +96   bytes per scanline (>= width*bpp/8)
	uint32_t framebuffer_width;// +100
	uint32_t framebuffer_height;//+104
	uint8_t  framebuffer_bpp;  // +108  bits per pixel
	uint8_t  framebuffer_type; // +109  0=indexed, 1=direct RGB, 2=EGA text
	uint8_t  color_info[6];    // +110  RGB field positions (type 1)
} __attribute__((packed));

// One entry of the memory map. `size` excludes itself, so the next entry is at
// (current + size + 4) — the classic Multiboot stride trap.
struct MmapEntry {
	uint32_t size;
	uint64_t base_addr;
	uint64_t length;
	uint32_t type;
} __attribute__((packed));

const uint32_t MB_FLAG_MEM = 1u << 0;   // mem_lower/mem_upper valid
const uint32_t MB_FLAG_MMAP = 1u << 6;  // mmap_length/mmap_addr valid
const uint32_t MB_FLAG_FRAMEBUFFER = 1u << 12;  // framebuffer_* valid (GRUB set a mode)
const uint32_t MMAP_TYPE_AVAILABLE = 1; // usable RAM

// A linear framebuffer GRUB handed us. Mirrors the relevant Multiboot fields.
struct MbFramebuffer {
	uint64_t addr;
	uint32_t pitch, width, height;
	uint8_t  bpp;
};

// Extract the framebuffer GRUB filled (flag bit 12), if it is a linear direct-RGB
// framebuffer (type 1). Returns false otherwise. Pure -> host-testable.
inline bool multibootFramebuffer(const MultibootInfo* mbi, MbFramebuffer* out) {
	if (!mbi || !(mbi->flags & MB_FLAG_FRAMEBUFFER))
		return false;
	if (mbi->framebuffer_type != 1)
		return false;
	out->addr = mbi->framebuffer_addr;
	out->pitch = mbi->framebuffer_pitch;
	out->width = mbi->framebuffer_width;
	out->height = mbi->framebuffer_height;
	out->bpp = mbi->framebuffer_bpp;
	return true;
}

}
```

- [ ] **Step 2: Utworzyć `arch/x86_64/boot/MultibootMmap.h`**

```cpp
/*
 * MultibootMmap.h (x86_64) — pure helpers over the Multiboot memory map: iterate entries and
 * compute the highest usable physical address. No hardware access — host-testable. Verbatim
 * mirror of arch/x86/boot/MultibootMmap.h.
 */
#pragma once
#include "MultibootInfo.h"

namespace kernel {

typedef void (*MmapCallback)(void* ctx, uint64_t base, uint64_t length, uint32_t type);

// Walk a raw mmap buffer of `len` bytes, invoking cb for each entry. The next
// entry sits at (current + entry.size + 4) — `size` excludes itself.
void parseMmapBuffer(const void* mmap, uint32_t len, void* ctx, MmapCallback cb);

// Highest usable physical byte (one past the top of the highest available
// region) found in a raw mmap buffer; 0 if none. Not clamped (returns u64).
uint64_t highestUsableInBuffer(const void* mmap, uint32_t len);

// Convenience: iterate the map referenced by `mbi` (no-op without the flag).
void parseMmap(const MultibootInfo* mbi, void* ctx, MmapCallback cb);

// Highest usable physical byte, clamped to 0xFFFFFFFF. Uses the mmap when
// present, else the mem_upper fallback, else 0.
uint32_t highestUsableAddr(const MultibootInfo* mbi);

}
```

- [ ] **Step 3: Utworzyć `arch/x86_64/boot/MultibootMmap.cpp`**

Wierna kopia x86 — używa już `uintptr_t`, więc jest LP64-clean (na x86_64 stride'uje po
8-bajtowych wskaźnikach poprawnie).

```cpp
#include "MultibootMmap.h"

namespace kernel {

void parseMmapBuffer(const void* mmap, uint32_t len, void* ctx, MmapCallback cb) {
	uintptr_t addr = (uintptr_t) mmap;
	uintptr_t end = addr + len;
	// Walk by (size + 4): `size` excludes the size field itself.
	while (addr + sizeof(uint32_t) <= end) {
		const MmapEntry* e = (const MmapEntry*) addr;
		cb(ctx, e->base_addr, e->length, e->type);
		addr += e->size + sizeof(uint32_t);
	}
}

namespace {
struct TopAcc {
	uint64_t top;
};
void accumulateTop(void* ctx, uint64_t base, uint64_t length, uint32_t type) {
	if (type != MMAP_TYPE_AVAILABLE)
		return;
	TopAcc* a = (TopAcc*) ctx;
	uint64_t regionEnd = base + length;
	if (regionEnd > a->top)
		a->top = regionEnd;
}
uint32_t clamp32(uint64_t v) {
	return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t) v;
}
}

uint64_t highestUsableInBuffer(const void* mmap, uint32_t len) {
	TopAcc acc = { 0 };
	parseMmapBuffer(mmap, len, &acc, accumulateTop);
	return acc.top;
}

void parseMmap(const MultibootInfo* mbi, void* ctx, MmapCallback cb) {
	if (!(mbi->flags & MB_FLAG_MMAP))
		return;
	parseMmapBuffer((const void*) (uintptr_t) mbi->mmap_addr, mbi->mmap_length, ctx, cb);
}

uint32_t highestUsableAddr(const MultibootInfo* mbi) {
	if (mbi->flags & MB_FLAG_MMAP)
		return clamp32(highestUsableInBuffer(
				(const void*) (uintptr_t) mbi->mmap_addr, mbi->mmap_length));
	if (mbi->flags & MB_FLAG_MEM)
		// mem_upper is KB above 1MB; total usable top = 1MB + mem_upper*1KB.
		return clamp32(0x100000ull + (uint64_t) mbi->mem_upper * 1024ull);
	return 0;
}

}
```

- [ ] **Step 4: Skompilować obiekt (weryfikacja składni elf64; bez `<string.h>`, więc bez copy-dance)**

```bash
docker run --rm -v "$(pwd)":/src -w /src nanos-build \
  x86_64-elf-g++ -c -ffreestanding -nostdlib -nostdinc++ --no-exceptions --no-rtti \
  -fno-leading-underscore -mno-red-zone -mno-sse -mno-mmx -mno-80387 \
  -Iarch/x86_64/boot arch/x86_64/boot/MultibootMmap.cpp -o /tmp/mm64.o && echo OK
```
Expected: `OK`.

- [ ] **Step 5: Commit**

```bash
git add arch/x86_64/boot/MultibootInfo.h arch/x86_64/boot/MultibootMmap.h arch/x86_64/boot/MultibootMmap.cpp
git commit -m "x86_64: add Multiboot info + memory-map parsing for the long-mode port"
```

---

## Task 2: `bootinfo_x86_64.cpp` — implementacja `<arch/bootinfo.h>`

Mirror `arch/x86/boot/bootinfo_x86.cpp`, ale wskaźnik Multiboot jest przekazywany **jawnie**
przez `bootSetMultibootInfo` (trampolina long-mode oddaje go w `rdi` jako arg0 `kentry64`),
nie zaszywany przez asm w globalu `mbd`. To symbol arch-wewnętrzny (oba końce w `arch/x86_64`),
więc **nie zmienia kontraktu MI** `<arch/bootinfo.h>`.

**Files:**
- Create: `arch/x86_64/boot/bootinfo_x86_64.cpp`

- [ ] **Step 1: Utworzyć `arch/x86_64/boot/bootinfo_x86_64.cpp`**

```cpp
/*
 * bootinfo_x86_64.cpp — x86_64 implementation of <arch/bootinfo.h> over Multiboot1.
 *
 * Mirrors arch/x86/boot/bootinfo_x86.cpp, but the Multiboot info pointer is handed over
 * explicitly by entry64 (the long-mode trampoline passes it in rdi as kentry64's arg0)
 * rather than stashed by the asm loader into a `mbd` global. The pointer and the mmap it
 * references are 32-bit physical addresses from GRUB, reachable through the loader's 1 GiB
 * identity map. Translates the Multiboot memory map into the arch-neutral usable-range
 * iteration the MI frame allocator consumes.
 */
#include <arch/bootinfo.h>
#include "MultibootInfo.h"
#include "MultibootMmap.h"

namespace {

// The Multiboot info pointer, set once by entry64 via bootSetMultibootInfo (below).
uintptr_t g_mbInfo = 0;

struct FwdCtx {
	void* ctx;
	arch::UsableRangeCb cb;
};

// parseMmap reports every entry; forward only the usable ones, dropping `type`.
void forwardUsable(void* c, uint64_t base, uint64_t length, uint32_t type) {
	if (type != kernel::MMAP_TYPE_AVAILABLE)
		return;
	FwdCtx* f = (FwdCtx*) c;
	f->cb(f->ctx, base, length);
}

}  // namespace

// Arch-internal (NOT an <arch/bootinfo.h> contract): entry64 calls this with kentry64's arg0.
extern "C" void bootSetMultibootInfo(unsigned long mb) {
	g_mbInfo = (uintptr_t) mb;
}

namespace arch {

uint32_t bootMemTop() {
	kernel::MultibootInfo* mbi = (kernel::MultibootInfo*) g_mbInfo;
	uint32_t top = mbi ? kernel::highestUsableAddr(mbi) : 0;
	return top ? top : 0x8000000;   // fallback: 128 MiB
}

void bootMemForEachUsable(void* ctx, UsableRangeCb cb) {
	FwdCtx f = { ctx, cb };
	kernel::MultibootInfo* mbi = (kernel::MultibootInfo*) g_mbInfo;
	if (mbi)
		kernel::parseMmap(mbi, &f, forwardUsable);
}

const BootFramebuffer* bootFramebuffer() {
	static BootFramebuffer fb;
	static int state = 0;   // 0=unprobed, 1=present, 2=absent
	if (state == 0) {
		kernel::MbFramebuffer mb;
		kernel::MultibootInfo* mbi = (kernel::MultibootInfo*) g_mbInfo;
		if (mbi && kernel::multibootFramebuffer(mbi, &mb)) {
			fb.addr = mb.addr;
			fb.pitch = mb.pitch;
			fb.width = mb.width;
			fb.height = mb.height;
			fb.bpp = mb.bpp;
			state = 1;
		} else {
			state = 2;
		}
	}
	return state == 1 ? &fb : 0;
}

}  // namespace arch
```

- [ ] **Step 2: Skompilować obiekt (bez `<string.h>` → bez copy-dance)**

```bash
docker run --rm -v "$(pwd)":/src -w /src nanos-build \
  x86_64-elf-g++ -c -ffreestanding -nostdlib -nostdinc++ --no-exceptions --no-rtti \
  -fno-leading-underscore -mno-red-zone -mno-sse -mno-mmx -mno-80387 \
  -Iarch/include -Iarch/x86_64/boot arch/x86_64/boot/bootinfo_x86_64.cpp -o /tmp/bi64.o && echo OK
```
Expected: `OK`.

- [ ] **Step 3: Commit**

```bash
git add arch/x86_64/boot/bootinfo_x86_64.cpp
git commit -m "x86_64: implement arch bootinfo over Multiboot (mmap + framebuffer)"
```

---

## Task 3: `console_x86_64.cpp` — sink VGA tekstowy

Samodzielny sink VGA 80x25 (@ 0xB8000, osiągalne przez identity-map z loadera). Port I/O dla
kursora przez inline asm (to kod MD, więc bramka `check-arch` go nie dotyczy). **Brak**
framebuffer-console (Linux fbcon) w Planie 2 — `consoleActivateFramebuffer` to no-op; przejęcie
przez FB dochodzi z pracami graficznymi w późniejszym planie. Dzięki temu staged link nie
ciągnie `FbConsole`/`Font8x16`/`vtk`.

**Files:**
- Create: `arch/x86_64/drivers/console_x86_64.cpp`

- [ ] **Step 1: Utworzyć `arch/x86_64/drivers/console_x86_64.cpp`**

```cpp
/*
 * console_x86_64.cpp — x86_64 VGA text-mode implementation of <arch/console.h>.
 *
 * 80x25 text buffer at physical 0xB8000 (reachable through the loader's identity map),
 * hardware cursor via VGA ports 0x3D4/0x3D5. A self-contained Plan-2 sink: no framebuffer
 * console yet (consoleActivateFramebuffer is a no-op; the fbcon takeover lands with the
 * graphics work in a later plan). Port I/O uses inline asm directly (this is MD code, so the
 * MI arch-cleanliness guard does not apply here).
 */
#include <arch/console.h>
#include <string.h>

namespace {

unsigned short cursorX = 0;
unsigned short cursorY = 0;
volatile unsigned short* videoram = (volatile unsigned short*) 0xB8000;

const unsigned short ATTR = 0x0F00;   // white on black, in the high byte of each cell

inline void outb(unsigned short port, unsigned char val) {
	__asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

void moveCursor() {
	unsigned short loc = (unsigned short) (cursorY * 80 + cursorX);
	outb(0x3D4, 14);
	outb(0x3D5, (unsigned char) (loc >> 8));
	outb(0x3D4, 15);
	outb(0x3D5, (unsigned char) loc);
}

void scroll() {
	if (cursorY >= 25) {
		memcpy((void*) videoram, (void*) (videoram + 80), 24 * 80 * 2);
		for (int i = 24 * 80; i < 25 * 80; i++)
			((unsigned short*) videoram)[i] = (unsigned short) (ATTR | ' ');
		cursorY = 24;
	}
}

}  // namespace

namespace arch {

void consolePutChar(char c) {
	if (c == 0x08 && cursorX) {
		cursorX--;
	} else if (c == 0x09) {
		cursorX = (unsigned short) ((cursorX + 8) & ~(8 - 1));
	} else if (c == '\r') {
		cursorX = 0;
	} else if (c == '\n') {
		cursorX = 0;
		cursorY++;
	} else if (c >= ' ') {
		videoram[cursorY * 80 + cursorX] = (unsigned short) (ATTR | (unsigned char) c);
		cursorX++;
	}
	if (cursorX >= 80) {
		cursorX = 0;
		cursorY++;
	}
	scroll();
	moveCursor();
}

void consoleClear() {
	const unsigned short blank = (unsigned short) (ATTR | ' ');
	for (int i = 0; i < 80 * 25; i++)
		videoram[i] = blank;
	cursorX = 0;
	cursorY = 0;
	moveCursor();
}

void consoleSetCursor(unsigned x, unsigned y) {
	cursorX = (unsigned short) x;
	cursorY = (unsigned short) y;
	moveCursor();
}

void consoleInit() {
	consoleClear();
}

// Plan 2: VGA text only. The framebuffer (fbcon) takeover lands with the graphics work.
void consoleActivateFramebuffer() {}

void consoleSize(unsigned* cols, unsigned* rows) {
	if (cols) *cols = 80;
	if (rows) *rows = 25;
}

}  // namespace arch
```

- [ ] **Step 2: Skompilować obiekt (zawiera `<string.h>` — użyj `-Iinclude` BEZ `-Ilib`, by uniknąć pułapki case-insensitive)**

```bash
docker run --rm -v "$(pwd)":/src -w /src nanos-build \
  x86_64-elf-g++ -c -ffreestanding -nostdlib -nostdinc++ --no-exceptions --no-rtti \
  -fno-leading-underscore -mno-red-zone -mno-sse -mno-mmx -mno-80387 \
  -Iarch/include -Iinclude arch/x86_64/drivers/console_x86_64.cpp -o /tmp/con64.o && echo OK
```
Expected: `OK`. (W realnym buildzie `_stage64` plik kompiluje się z `$(KSRC)` na FS
case-sensitive, więc pełne `KINCLUDES` z `-Ilib` są tam bezpieczne.)

- [ ] **Step 3: Commit**

```bash
git add arch/x86_64/drivers/console_x86_64.cpp
git commit -m "x86_64: add VGA text-mode console sink (arch/console.h)"
```

---

## Task 4: Minimalna poprawka LP64 w `Console` — `writeHex(unsigned long)`

`Console::writeHex(int)` obcina 64-bitowy adres. Dodajemy **jedno** przeciążenie
`writeHex(unsigned long)` (szerokość zależna od architektury: 32-bit na i686, 64-bit na
x86_64). To JEDYNA zmiana MI w Planie 2. Wszystkie istniejące wywołania mają jawne `(int)`
(zweryfikowane: `kernel/Kernel.cpp`, `arch/x86/cpu/fault_x86.cpp`), więc przeciążenie nie
wprowadza dwuznaczności na i686. **Pełny LP64-sweep MI — Plan 7.**

**Files:**
- Modify: `drivers/Console.h`
- Modify: `drivers/Console.cpp`

- [ ] **Step 1: Dodać deklarację w `drivers/Console.h`**

Po linii `static void writeHex(int hex);` dodaj:
```cpp
		static void writeHex(unsigned long hex);   // 64-bit-capable on LP64 (addresses)
```

Kontekst (fragment po edycji):
```cpp
		static void write(int d);
		static void writeHex(int hex);
		static void writeHex(unsigned long hex);   // 64-bit-capable on LP64 (addresses)
		static void write(const char* text);
```

- [ ] **Step 2: Dodać implementację w `drivers/Console.cpp`**

Po metodzie `Console::writeHex(int)` (kończy się na `	write(ss);\n}`) dodaj:
```cpp
// 64-bit-capable hex (the int overload truncates a 64-bit address). Width follows the
// platform: 32 bits on i686 (ILP32), 64 bits on x86_64 (LP64). Prints "0x" then the value
// with no leading zeros (a lone 0 still prints "0x0"). Part of the minimal LP64 fix; the
// full MI sweep is Plan 7.
void Console::writeHex(unsigned long v) {
	write("0x");
	bool started = false;
	for (int shift = (int) (sizeof(unsigned long) * 8) - 4; shift >= 0; shift -= 4) {
		unsigned digit = (unsigned) ((v >> shift) & 0xFUL);
		if (digit != 0 || started || shift == 0) {
			write((char) (digit < 10 ? '0' + digit : 'a' + digit - 10));
			started = true;
		}
	}
}
```

- [ ] **Step 3: Regresja i686 — kernel buduje się, MI nadal arch-clean**

Run: `make build && make check-arch`
Expected: kernel i686 buduje się (nowe przeciążenie nie psuje żadnego wywołania);
`OK: MI layer is arch-clean.`

- [ ] **Step 4: Host-testy (sito LP64 — host buduje 64-bit, więc przeciążenie jest tam ćwiczone)**

Run: `make test`
Expected: cały doctest suite zielony, bramka pokrycia spełniona (nowa metoda nie obniża
pokrycia poniżej progu — jest cienka).

- [ ] **Step 5: Commit**

```bash
git add drivers/Console.h drivers/Console.cpp
git commit -m "console: add 64-bit-capable writeHex(unsigned long) overload"
```

---

## Task 5: `bringup_stubs64.cpp` — shimy CPU dla linkowania staged heapu

`mm/memory_manager.cpp` (`heapPanic`) odwołuje się do `arch::cpuDisableInterrupts()` i
`arch::cpuHalt()`. Te symbole muszą się **rozwiązać przy linkowaniu** staged obrazu, choć
ścieżka panic nie jest wołana. Pełny `arch/x86_64/cpu/cpu_x86_64.cpp` to Plan 4 — tu dajemy
minimalne, **poprawne** shimy, linkowane TYLKO do obrazu staged (nigdy do pełnego kernela),
więc nie powstaje kolizja symboli w Planie 4/5.

**Files:**
- Create: `arch/x86_64/boot/bringup_stubs64.cpp`

- [ ] **Step 1: Utworzyć `arch/x86_64/boot/bringup_stubs64.cpp`**

```cpp
/*
 * bringup_stubs64.cpp — Plan-2 staging shims for the few <arch/cpu.h> symbols the MI byte
 * heap (mm/memory_manager.cpp heapPanic) references at link time but that the full arch CPU
 * layer does not yet provide. Only the two actually referenced are defined; the real, full
 * arch/x86_64/cpu/cpu_x86_64.cpp (Plan 4) supersedes this file, which is linked ONLY into the
 * staged bring-up image, never the full kernel — so there is no duplicate-symbol clash later.
 */
#include <arch/cpu.h>

namespace arch {

void cpuDisableInterrupts() {
	__asm__ __volatile__("cli");
}

void cpuHalt() {
	__asm__ __volatile__("hlt");
}

}  // namespace arch
```

- [ ] **Step 2: Skompilować obiekt**

```bash
docker run --rm -v "$(pwd)":/src -w /src nanos-build \
  x86_64-elf-g++ -c -ffreestanding -nostdlib -nostdinc++ --no-exceptions --no-rtti \
  -fno-leading-underscore -mno-red-zone -mno-sse -mno-mmx -mno-80387 \
  -Iarch/include arch/x86_64/boot/bringup_stubs64.cpp -o /tmp/stub64.o && echo OK
```
Expected: `OK`.

- [ ] **Step 3: Commit**

```bash
git add arch/x86_64/boot/bringup_stubs64.cpp
git commit -m "x86_64: add staged cpu shims (cli/hlt) for the bring-up heap link"
```

---

## Task 6: `kentry64` woła MI `kmain()` + okrojony `Kernel::start`

`kmain()` (MI, `init/kmain.cpp`) jest niezmienione: konstruuje `Kernel` i woła `start()`. Dla
buildu staged dostarczamy **inną, minimalną** definicję `kernel::Kernel::start` w
`arch/x86_64/boot/KernelStage64.cpp` — linkowana tylko do obrazu staged, więc nie koliduje z
prawdziwym `kernel/Kernel.cpp` (różne zestawy linkowania). `entry64.cpp` przestaje pisać po
VGA wprost — oddaje wskaźnik Multiboot do `bootinfo` i woła `kmain()`.

**Files:**
- Modify: `arch/x86_64/boot/entry64.cpp`
- Create: `arch/x86_64/boot/KernelStage64.cpp`

- [ ] **Step 1: Przepisać `arch/x86_64/boot/entry64.cpp` (zastępuje proof-of-life z Planu 1)**

```cpp
// arch/x86_64/boot/entry64.cpp — 64-bit C++ entry reached from the long-mode trampoline
// (loader.S, which leaves the Multiboot info pointer in rdi = arg0). Plan 2: hand that
// pointer to the bootinfo layer, then call the machine-independent kmain(), which runs the
// staged Kernel::start. (Plan 1's direct VGA proof-of-life is now replaced by the real MI
// console path.)

extern "C" void kmain();                       // MI entry (init/kmain.cpp)
extern "C" void bootSetMultibootInfo(unsigned long mb);  // arch-internal (bootinfo_x86_64)

extern "C" void kentry64(unsigned long mb_info) {
	bootSetMultibootInfo(mb_info);             // stash for bootinfo_x86_64 (mmap/framebuffer)
	kmain();                                    // -> staged kernel::Kernel::start()
	for (;;)                                     // backstop: kmain() must not return
		__asm__ __volatile__("hlt");
}
```

- [ ] **Step 2: Utworzyć `arch/x86_64/boot/KernelStage64.cpp`**

```cpp
// arch/x86_64/boot/KernelStage64.cpp — Plan-2 STAGED minimal kernel::Kernel::start for the
// x86_64 bring-up. Deliberately NOT the full kernel/Kernel.cpp (which drags in the storage
// stack, scheduler, syscalls and every arch contract not yet ported). This staged start
// proves the long-mode kernel can: reach MI kmain(), drive the real MI Console formatting
// layer over the x86_64 VGA sink, read the Multiboot memory map parsed in 64-bit
// (arch::bootMemTop / bootFramebuffer), exercise the MI byte heap (memory_manager) under
// LP64, then idle. Linked ONLY into the staged image; the real kernel/Kernel.cpp supersedes
// it once Plans 3-5 land the missing arch layers (paging/interrupts/storage). The idle
// hlt-loop uses inline asm directly — this is MD-located code, exempt from the MI guard.
#include "Kernel.h"
#include "Console.h"
#include "memory_manager.h"
#include <arch/console.h>
#include <arch/bootinfo.h>

namespace kernel {

// A small scratch heap inside the loader's 1 GiB identity map (the kernel lives at 1 MiB and
// is tiny). 16 MiB base, 1 MiB arena — comfortably within QEMU's 512 MiB and the identity map.
static const unsigned long STAGE_HEAP_BASE = 0x01000000UL;   // 16 MiB
static const unsigned      STAGE_HEAP_SIZE = 0x00100000U;    // 1 MiB

void Kernel::start() {
	Console::clearScreen();
	Console::writeLine("");
	Console::writeLine("    NanOS x86_64  --  staged bring-up (Plan 2)");
	Console::writeLine("");
	Console::writeLine("[ OK ] long mode + MI kmain() reached");
	Console::writeLine("[ OK ] MI Console formatting over the x86_64 VGA sink");

	// Memory map parsed by bootinfo_x86_64 from the Multiboot1 info (64-bit walk).
	unsigned long top = (unsigned long) arch::bootMemTop();
	Console::write("  RAM top: ");
	Console::writeHex(top);
	Console::write("  (");
	Console::write((int) (top / (1024UL * 1024UL)));
	Console::writeLine(" MiB usable)");

	// Framebuffer (none under the Plan-1 multiboot header, which requests no video mode).
	const arch::BootFramebuffer* fb = arch::bootFramebuffer();
	if (fb) {
		Console::write("  Framebuffer @ ");
		Console::writeHex((unsigned long) fb->addr);
		Console::write("  ");
		Console::write((int) fb->width);
		Console::write("x");
		Console::write((int) fb->height);
		Console::write("x");
		Console::writeLine((int) fb->bpp);
	} else {
		Console::writeLine("  Framebuffer: none (VGA text mode)");
	}

	// Exercise the MI byte heap under LP64 (8-byte pointers): lay out the arena, allocate,
	// print the (64-bit-capable) pointer, free.
	heapInit((void*) STAGE_HEAP_BASE, STAGE_HEAP_SIZE);
	void* p = malloc(128);
	Console::write("  heap: malloc(128) -> ");
	Console::writeHex((unsigned long) p);
	Console::writeLine("");
	free(p);

	// Proof the 64-bit hex path prints the upper 32 bits (the narrow Console LP64 fix).
	Console::write("  hex64 check: ");
	Console::writeHex((unsigned long) 0x123456789ABCUL);
	Console::writeLine("");

	Console::writeLine("");
	Console::writeLine("[ idle ] staged kernel parked (hlt loop)");

	for (;;)
		__asm__ __volatile__("hlt");
}

}  // namespace kernel
```

- [ ] **Step 3: Skompilować oba obiekty (weryfikacja składni)**

`entry64.cpp` nie ma includów — kompiluje się bezpośrednio:
```bash
docker run --rm -v "$(pwd)":/src -w /src nanos-build \
  x86_64-elf-g++ -c -ffreestanding -nostdlib -nostdinc++ --no-exceptions --no-rtti \
  -fno-leading-underscore -mno-red-zone -mno-sse -mno-mmx -mno-80387 \
  arch/x86_64/boot/entry64.cpp -o /tmp/entry64.o && echo OK
```
`KernelStage64.cpp` zawiera `Console.h`/`memory_manager.h` (→ `<string.h>`) — użyj
`-Iinclude` BEZ `-Ilib`:
```bash
docker run --rm -v "$(pwd)":/src -w /src nanos-build \
  x86_64-elf-g++ -c -ffreestanding -nostdlib -nostdinc++ --no-exceptions --no-rtti \
  -fno-leading-underscore -mno-red-zone -mno-sse -mno-mmx -mno-80387 \
  -Iarch/include -Ikernel -Idrivers -Imm -Iinclude \
  arch/x86_64/boot/KernelStage64.cpp -o /tmp/kstage64.o && echo OK
```
Expected: oba wypisują `OK`.

- [ ] **Step 4: Commit**

```bash
git add arch/x86_64/boot/entry64.cpp arch/x86_64/boot/KernelStage64.cpp
git commit -m "x86_64: kentry64 calls MI kmain; add staged minimal Kernel::start"
```

---

## Task 7: Makefile — staged link (osobny `bin/stage64/`) + przepisany `_bringup64`

Plan 1 linkował tylko `loader64.o + entry64.o` jednym jawnym recipe. Plan 2 dokłada obiekty MI
(`Console.o`, `memory_manager.o`, `Heap.o`, `string_funcs.o`, `icxxabi.o`, `kmain.o`) — które
mają wspólne basename'y z buildem i686. Aby elf64 nie zatruł `bin/*.o` (elf32) i686 i nie było
niespójności mtime przy przełączaniu `ARCH`, staged obiekty idą do **`bin/stage64/`**. Kompilacja
przez istniejący copy-dance (`$(KSRC)`), bo `Console`/`memory_manager` mają `#include <string.h>`.

**Files:**
- Modify: `Makefile` (sekcja kontenerowa: nowe zmienne + reguły + `_stage64`; przepisany
  `_bringup64`; sekcja host: aktualizacja echo w `bringup64`)

- [ ] **Step 1: Dodać zmienne + reguły staged (sekcja kontenerowa, po regule `$(BINFOLDER)%.o: %.S` z `Makefile:454`)**

Wstaw po regule NASM (linia z `nasm -f $(ASM_FMT) $< -o $@`, `Makefile:454`):
```makefile

# ---- x86_64 Plan 2 staged bring-up ------------------------------------------------------
# The staged image links a MINIMAL slice of MI + x86_64 MD (Console + memory_manager +
# bootinfo + a staged Kernel::start) — NOT the full kernel. Its objects are elf64 and MUST NOT
# share bin/ with the i686 elf32 objects of the SAME basename (Console.o, memory_manager.o,
# string_funcs.o, kmain.o, ...), so they build into a SEPARATE dir bin/stage64/. That also
# sidesteps stale-mtime format mismatches when switching ARCH between full builds.
STAGE_BIN=$(BINFOLDER)stage64/
STAGE64_OBJS=loader64.o entry64.o console_x86_64.o bringup_stubs64.o bootinfo_x86_64.o \
             MultibootMmap.o kmain.o KernelStage64.o Console.o memory_manager.o Heap.o \
             string_funcs.o icxxabi.o
STAGE64_PATHS=$(addprefix $(STAGE_BIN),$(STAGE64_OBJS))

# Staged compile rules write into bin/stage64/ (NOT bin/). The stage pattern's stem is shorter
# than the generic bin/%.o pattern's for a bin/stage64/X.o target, so make prefers it.
$(STAGE_BIN)%.o: %.cpp
	@mkdir -p $(STAGE_BIN)
	$(CXX) -c $(CXXFLAGS) -MMD -MP $< -o $@
# loader64.o from loader.S: the source basename is `loader`, but the 64-suffixed object keeps
# the elf64 boot object from ever colliding with i686's bin/loader.o (an explicit recipe, not
# the generic %.S rule, because basenames differ).
$(STAGE_BIN)loader64.o: arch/x86_64/boot/loader.S
	@mkdir -p $(STAGE_BIN)
	nasm -f $(ASM_FMT) $< -o $@

# Link the staged set. -lgcc covers any compiler helper routines ($(LD) = $(CROSS)gcc).
_stage64: $(STAGE64_PATHS)
	$(LD) -T$(ARCH_LINKER) -nostdlib -nostartfiles -o $(BINFOLDER)kernel64.bin $(STAGE64_PATHS) -lgcc

-include $(STAGE64_PATHS:.o=.d)
```

- [ ] **Step 2: Przepisać kontenerowy cel `_bringup64` (zastępuje Plan-1 recipe z `Makefile:463-471`)**

Zamień cały blok `_bringup64:` (od `_bringup64:` do `grub-mkrescue -o $(BINFOLDER)nanos64.iso /tmp/iso64`) na:
```makefile
# Plan 2: build the STAGED long-mode kernel (MI Console + memory_manager + bootinfo + staged
# Kernel::start), then wrap it in the GRUB rescue ISO. Sources are compiled from a
# case-sensitive copy ($(KSRC)) because Console/memory_manager #include <string.h> (the
# bind-mounted macOS FS is case-insensitive, where it would collide with lib/String.h). The
# objects land in /src/bin/stage64 via the $(KSRC)/bin -> /src/bin symlink, isolated from the
# i686 elf32 objects in /src/bin.
_bringup64:
	@mkdir -p $(STAGE_BIN)
	@rm -rf $(KSRC) && mkdir -p $(KSRC) && \
	 tar -cf - --exclude=.git --exclude=disk --exclude=bin --exclude=iso --exclude=coverage --exclude=tests -C /src . | tar -xf - -C $(KSRC) && \
	 ln -s /src/$(BINFOLDER) $(KSRC)/bin
	$(MAKE) -C $(KSRC) _stage64
	@rm -rf /tmp/iso64 && mkdir -p /tmp/iso64/boot/grub
	@cp $(BINFOLDER)kernel64.bin /tmp/iso64/boot/kernel64.bin
	@printf 'set timeout=0\nset default=0\nmenuentry "nanos64" {\n  multiboot /boot/kernel64.bin\n  boot\n}\n' > /tmp/iso64/boot/grub/grub.cfg
	grub-mkrescue -o $(BINFOLDER)nanos64.iso /tmp/iso64
```

- [ ] **Step 3: Zaktualizować echo w host-owym `bringup64` (`Makefile:376`)**

Zamień (`Makefile:376`):
```makefile
	@echo "Booting bin/nanos64.iso — expect 'NanOS x86_64 long mode OK' on the VGA console."
```
na:
```makefile
	@echo "Booting bin/nanos64.iso — expect the staged banner ('NanOS x86_64 -- staged bring-up') on the VGA console."
```

- [ ] **Step 4: Zbudować staged obraz w kontenerze**

```bash
docker run --rm -v "$(pwd)":/src -w /src nanos-build make ARCH=x86_64 _bringup64
ls -l bin/kernel64.bin bin/nanos64.iso
```
Expected: powstają `bin/kernel64.bin` i `bin/nanos64.iso`; w `bin/stage64/` leżą staged
obiekty elf64.

- [ ] **Step 5: Potwierdzić, że kernel to multiboot ELF64 z entry @ ~1 MiB**

```bash
docker run --rm -v "$(pwd)":/src -w /src nanos-build sh -c \
  'x86_64-elf-readelf -h bin/kernel64.bin | grep -E "Class|Machine|Entry"'
```
Expected:
```
Class:                             ELF64
Machine:                           Advanced Micro Devices X86-64
Entry point address:               0x10001?
```
(Entry przesunięte o nagłówek `.boot`/multiboot względem 0x100000 — jak w Planie 1, poprawne.)

- [ ] **Step 6: Commit**

```bash
git add Makefile
git commit -m "build: stage x86_64 bring-up link in bin/stage64 + reach MI kmain"
```

---

## Task 8: Weryfikacja boot w QEMU (headless) + regresja i686 + domknięcie

**Files:** (brak nowych — używa celów z Tasków 1-7)

- [ ] **Step 1: Boot interaktywny (opcjonalnie — szybki ogląd)**

```bash
qemu-system-x86_64 -cpu qemu64 -m 512 -cdrom bin/nanos64.iso
```
Expected: w oknie QEMU wieloliniowy banner: `NanOS x86_64 -- staged bring-up (Plan 2)`,
`[ OK ] long mode + MI kmain() reached`, linia `RAM top: 0x...  (511 MiB usable)`,
`Framebuffer: none (VGA text mode)`, `heap: malloc(128) -> 0x...`,
`hex64 check: 0x123456789abc`, `[ idle ] staged kernel parked`. Maszyna wisi na `hlt`.

- [ ] **Step 2: Weryfikacja headless (powtarzalna, wzorzec z CLAUDE.md / Plan 1 Task 6)**

```bash
qemu-system-x86_64 -cpu qemu64 -m 512 -cdrom bin/nanos64.iso \
  -no-reboot -d int -D /tmp/qlog64 \
  -display none -monitor unix:/tmp/qmon64,server,nowait &
QEMU_PID=$!
sleep 3
python3 - <<'PY'
import socket, time
s = socket.socket(socket.AF_UNIX); s.connect("/tmp/qmon64")
s.recv(4096)
s.sendall(b"screendump /tmp/x86_64-plan2.ppm\n")
time.sleep(1)
PY
kill $QEMU_PID 2>/dev/null
sips -s format png /tmp/x86_64-plan2.ppm --out /tmp/x86_64-plan2.png >/dev/null
echo "Open /tmp/x86_64-plan2.png — expect the multi-line staged banner."
```
Expected: PNG pokazuje wieloliniowy banner z Step 1.

- [ ] **Step 3: Potwierdzić ZERO faultów (kryterium wyjścia)**

```bash
grep -E 'v=0d|v=08|v=0e' /tmp/qlog64 && echo "FAULT DETECTED" || echo "no GP/DF/PF faults"
```
Expected: `no GP/DF/PF faults`. (Gdyby pojawił się `v=0d`/`v=08`/`v=0e` — odpowiednio GP /
podwójny / page-fault — sprawdź: identity-map z loadera obejmuje 0xB8000 i region heapu
0x01000000–0x01100000, oraz że `g_mbInfo` wskazuje na osiągalny obszar < 1 GiB.)

- [ ] **Step 4: Regresja i686 (brak regresji + MI arch-clean + host-testy)**

```bash
make build && make check-arch && make test
```
Expected: kernel i686 buduje się; `OK: MI layer is arch-clean.`; cały doctest suite zielony.

- [ ] **Step 5: Domknięcie planu — adnotacja w specu (kamień 2)**

W `docs/superpowers/specs/2026-06-15-x86_64-migration-analysis.md`, §7, dopisać przy kamieniu 2
adnotację „✅ pełny boot + konsola + MI kmain zrealizowane w
plans/2026-06-15-x86_64-plan-2-boot-console-kmain.md". (Spec jest plikiem **untracked**
wykluczonym ze stage'owania — jak w Planie 1 — więc odhaczenie zapisujemy też tutaj, a sama
edycja specu nie wchodzi do commita.)

- [ ] **Step 6: Commit (pusty marker domknięcia, jak w Planie 1)**

```bash
git commit --allow-empty -m "x86_64: verify staged long-mode kernel reaches MI kmain + banner in QEMU"
```

---

## Self-Review (wykonane)

- **Spec coverage:** Plan realizuje kamień 2 z §7 specu (pełny boot do long mode + konsola +
  wejście w MI `kmain`) oraz dotyka §2.1 (parsing Multiboot w 64-bit przez `bootinfo_x86_64`)
  i §2.4 (sink VGA `console_x86_64`). LP64-sweep (§4) świadomie odłożony do Planu 7 — Plan 2
  robi tylko minimalny podzbiór: jedno przeciążenie `Console::writeHex(unsigned long)`; cast
  `(unsigned)` w `memory_manager` zostawiony nietknięty (poprawny dla małego staged heapu).
- **Placeholders:** brak — każdy krok ma realny kod / komendę / oczekiwany wynik; żadnego
  „TODO"/„similar to" — kod powtórzony w całości (m.in. wierne kopie Multiboot z x86).
- **Type/naming consistency vs kontrakt:**
  - `kentry64(unsigned long mb_info)` — sygnatura niezmieniona względem Planu 1 (loader.S
    `extern kentry64` nadal pasuje); MI `kmain()` (bez argumentów) niezmienione, wskaźnik
    Multiboot płynie ścieżką bootinfo (`bootSetMultibootInfo`), nie przez sygnaturę MI.
  - katalogi `arch/x86_64/{boot,drivers}`, prefiks `x86_64-elf-`, `ASM_FMT=elf64`,
    `QEMU=qemu-system-x86_64`, `QEMU_CPU=-cpu qemu64`, `KARCHFLAGS` — zgodne z `arch.mk`.
  - obiekt bootowy `loader64.o` (sufiks `64` przeciw kolizji z i686 `bin/loader.o`); nowe
    pliki sink/bootinfo nazwane `console_x86_64.cpp` / `bootinfo_x86_64.cpp` zgodnie z
    kontraktem; staged obiekty MI izolowane w `bin/stage64/` (elf64 vs elf32).
  - boot przez GRUB rescue-ISO (`-cdrom bin/nanos64.iso`), nie `qemu -kernel` (multiboot1
    odrzuca ELF64) — jawnie wybrane, zgodnie z Planem 1.
- **Brak regresji / arch-clean:** zmiana MI ograniczona do przeciążenia `writeHex` (bez asm,
  bez nagłówków x86) — `make check-arch` zostaje zielone; wszystkie istniejące wywołania
  `writeHex` mają jawne `(int)` (zweryfikowane grepem), więc brak dwuznaczności na i686.

---

## Zależności / preconditions

- **Wymaga wylądowanego Planu 1** (`feat/x86_64-foundation`, commity b986cb4..d9f514d): drugi
  toolchain `x86_64-elf` w obrazie `nanos-build`, `arch/x86_64/arch.mk` + `linker.ld`,
  `arch/x86_64/boot/loader.S` (trampolina long-mode pozostawiająca wskaźnik Multiboot w `rdi`),
  sparametryzowany Makefile (`ASM_FMT`/`QEMU`/`QEMU_CPU`/`KARCHFLAGS`) oraz host-owy cel
  `bringup64` bootujący `bin/nanos64.iso`.
- Plan 2 **nie modyfikuje** `loader.S` ani `arch.mk`/`linker.ld` — ewoluuje wyłącznie
  `entry64.cpp` i dokłada sink/bootinfo/staged-start + zmiany w Makefile.
- Pracujemy na gałęzi feature (kontynuacja `feat/x86_64-foundation` lub `feat/x86_64-plan2`),
  nie na `master`. Commity bez jakiejkolwiek wzmianki o Claude/AI.

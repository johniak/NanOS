# x86_64 Plan 4 — GDT/IDT/TSS 64-bit + przerwania

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Dostarczyć prawdziwe 64-bitowe tablice deskryptorów i ścieżkę przerwań dla
`arch/x86_64/`: **GDT** (kod/dane + **16-bajtowy deskryptor TSS** z 64-bitową bazą),
**IDT** (256 16-bajtowych gatów z polem IST, PIC zremapowany na 0x20/0x28), **64-bitowy
TSS** (`rsp0/1/2`, `ist1..7`, `iomap_base`), nowy `Registers` (TrapFrame: `rax…r15`,
`rip/cs/rflags/rsp/ss`, `int_no/err`) oraz przepisane `isr64.S`/`irq64.S` z **ręcznym
zrzutem 15 rejestrów**, `swapgs` na granicy ring3↔ring0, `iretq` i poszanowaniem **red
zone (128 B)**. Wpinamy klawiaturę (IRQ1) i PIT (IRQ0) oraz raportowanie #PF/#GP przez
CR2.

**Architecture:** Migracja-zastąpienie (spec `docs/superpowers/specs/2026-06-15-x86_64-migration-analysis.md`,
§0.1). Plan 4 wypełnia katalog `arch/x86_64/cpu/` — równoległą implementację tych samych
kontraktów (`<arch/cpu.h>`, `<arch/irq.h>`) co `arch/x86/cpu/`, **bez ruszania i686**
(oba zielone w okresie przejściowym). Buduje na **staged** modelu Planu 2 (obiekty elf64
w `bin/stage64/`, link minimalnego MI + MD) i na **Planie 3** (`mmuInitKernel` już
zbudował własny PML4 identity-mapujący RAM, więc IDT i handlery leżą w zmapowanej
pamięci). Przerwania włączamy w okrojonym `Kernel::start` (z Planu 2), tuż za
`mmuInitKernel` (z Planu 3) — `cpuInit()` instaluje GDT/IDT/TSS, self-test wpina PIT +
klawiaturę, `sti`, idle.

**Tech Stack:** `x86_64-elf` cross-toolchain (binutils 2.43 + gcc 14.2.0), NASM
(`-f elf64`), GNU ld, QEMU (`qemu-system-x86_64`, boot GRUB ISO), Docker (`nanos-build`).
Wzorce kanoniczne OSDev: „64-bit GDT", „Interrupt Descriptor Table (long mode)",
„Task State Segment (x86-64)", „swapgs".

**Reference:** spec §2.2 (`Gdt`/`Idt`/`Tss`/`Interrupt(Registers)`/`isr.S`/`irq.S`/
`cpu_x86`/`fault_x86`), §7 kamień 4, §8 (red zone, `swapgs`, adresy kanoniczne).
i686 do odwzorowania: `arch/x86/cpu/{Gdt,Idt,Tss,Interrupt,cpu_x86,fault_x86,irq_x86}.*`,
`arch/x86/cpu/{isr,irq}.S`.

**Konwencja nazw (zgodna z kontraktem Planów 1-3, 5-8):** obiekty x86_64 o basename
kolidującym z i686 w współdzielonym `bin/` dostają **sufiks `64`** — źródła nazywamy od
razu `Gdt64`/`Idt64`/`Interrupt64`/`isr64.S`/`irq64.S`, więc reguła `%.o` daje
`Gdt64.o`/`Idt64.o`/`Interrupt64.o`/`isr64.o`/`irq64.o` bez kolizji (a staged build
Planu 2 i tak izoluje je w `bin/stage64/`). Pliki o unikalnym basename
(`cpu_x86_64.cpp`, `fault_x86_64.cpp`, `irq_x86_64.cpp`, `irqtest64.cpp`, `Port64.h`,
`Tss64.h`) zachowują naturalne nazwy. TrapFrame nazywa się `Registers` (mirror i686), by
opaque `arch::TrapFrame` z `<arch/irq.h>` i `registerIrqHandler` pozostały
źródłowo-zgodne; pola są `uint64_t`.

---

## File Structure

| Plik | Odpowiedzialność | Akcja |
|---|---|---|
| `arch/x86_64/cpu/Port64.h` | header-only inline port I/O (`outb`/`inb`/`inw`) — PIC remap, PIT, klawiatura | Create |
| `arch/x86_64/cpu/Tss64.h` | 64-bit TSS (104 B): `rsp0/1/2`, `ist1..7`, `iomap_base`; brak `ss0`/GPR | Create |
| `arch/x86_64/cpu/Gdt64.h` | `GdtEntry64` (8 B) + `TssDescriptor64` (16 B, baza 64-bit) + `Gdt64Ptr` + klasa `Gdt64` | Create |
| `arch/x86_64/cpu/Gdt64.cpp` | składa GDT (null/kcode/kdata/ucode/udata + 16-B TSS @ 0x28), IST1 dla #DF, `lgdt`+reload CS przez `lretq`, `ltr` | Create |
| `arch/x86_64/cpu/Idt64.h` | `IdtEntry64` (16 B: `base_lo/mid/hi`, 3-bit IST) + `Gdt64Ptr`-pokrewny `Idt64Ptr` + klasa `Idt64` + externy `isr0..31`/`irq0..15` | Create |
| `arch/x86_64/cpu/Idt64.cpp` | remap PIC 0x20/0x28, 256 gatów (#DF na IST1, reszta IST0), `lidt` | Create |
| `arch/x86_64/cpu/Interrupt64.h` | `Registers` (TrapFrame `uint64_t`), `IsrHandler`, klasa `Interrupt`, `IRQ0..15` | Create |
| `arch/x86_64/cpu/Interrupt64.cpp` | tablica handlerów + `isr_handler(Registers*)` / `irq_handler(Registers*)` (EOI do PIC) | Create |
| `arch/x86_64/cpu/isr64.S` | NASM elf64: stuby 0-31 (err/no-err), wspólny dispatch — ręczny zrzut 15 GPR, `swapgs`, `iretq` | Create |
| `arch/x86_64/cpu/irq64.S` | NASM elf64: stuby 32-47, wspólny dispatch → `irq_handler`, `iretq` | Create |
| `arch/x86_64/cpu/cpu_x86_64.cpp` | impl `<arch/cpu.h>`: `cpuInit` (GDT/IDT/TSS), `cpuDisable/Enable/Halt`, `cpuIrqSave/Restore` (RFLAGS via `pushfq`), `archLoadThreadTls` (stub do Planu 6) | Create |
| `arch/x86_64/cpu/irq_x86_64.cpp` | impl `<arch/irq.h>`: `registerIrqHandler`/`registerTrapHandler` (most do `kernel::Interrupt`) | Create |
| `arch/x86_64/cpu/fault_x86_64.cpp` | handlery #GP (13) / #PF (14): zrzut rejestrów + CR2 (64-bit), halt | Create |
| `arch/x86_64/cpu/irqtest64.cpp` | bring-up self-test: PIT @ 100 Hz (IRQ0, kropka/s) + klawiatura (IRQ1, echo scancode); `irqSelfTest()` | Create |
| `arch/x86_64/boot/KernelStage64.cpp` | (Plan 2) dołożyć `arch::cpuInit()` + `irqSelfTest()` + `sti` przed pętlą idle | Modify |
| `arch/x86_64/arch.mk` | dopisać obiekty cpu do `ARCH_SOURCES` | Modify |
| `Makefile` | dopisać obiekty cpu do `STAGE64_OBJS` + staged reguła `$(STAGE_BIN)%.o: %.S` (NASM) | Modify |

---

## Task 1: 64-bit GDT + TSS (`Port64.h`, `Tss64.h`, `Gdt64.{h,cpp}`)

W long mode bazy/limity deskryptorów danych i kodu są ignorowane — liczy się bit **L**
(64-bit code). Jedyny „gruby" wpis to **16-bajtowy deskryptor TSS** (baza 64-bit).

**Files:**
- Create: `arch/x86_64/cpu/Port64.h`
- Create: `arch/x86_64/cpu/Tss64.h`
- Create: `arch/x86_64/cpu/Gdt64.h`
- Create: `arch/x86_64/cpu/Gdt64.cpp`

- [ ] **Step 1: Utworzyć `arch/x86_64/cpu/Port64.h`**

```cpp
// arch/x86_64/cpu/Port64.h — header-only x86 port I/O for the x86_64 cpu layer.
// Port I/O semantics are identical to i686 (long mode changes nothing here); kept inline
// and header-only so Idt/PIT/keyboard bring-up don't need a separate translation unit.
#pragma once

namespace kernel {

inline void port_outb(unsigned short port, unsigned char value) {
    __asm__ __volatile__("outb %0, %1" : : "a"(value), "Nd"(port));
}
inline unsigned char port_inb(unsigned short port) {
    unsigned char v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
inline unsigned short port_inw(unsigned short port) {
    unsigned short v;
    __asm__ __volatile__("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

}  // namespace kernel
```

- [ ] **Step 2: Utworzyć `arch/x86_64/cpu/Tss64.h`**

```cpp
/*
 * Tss64.h — x86-64 Task State Segment (104 bytes).
 *
 * Long mode drops the 32-bit TSS's saved GPRs/segments and ss0. The CPU uses only:
 *   - rsp0/1/2: the stack to switch to on an interrupt that raises CPL (ring3->ring0),
 *   - ist1..ist7: the Interrupt Stack Table — gates with a non-zero IST index land on a
 *     known-good stack regardless of the interrupted rsp (we use ist1 for #DF),
 *   - iomap_base: offset to the I/O permission bitmap; == sizeof(Tss64) means "none".
 * All fields are at architecture-fixed offsets (rsp0 @ 4); the struct is packed so the
 * compiler can't insert padding that would shift them.
 */
#pragma once
#include <stdint.h>

namespace kernel {

struct Tss64 {
    uint32_t reserved0;                                  // 0
    uint64_t rsp0;                                       // 4   kernel stack for ring3->ring0
    uint64_t rsp1;                                       // 12
    uint64_t rsp2;                                       // 20
    uint64_t reserved1;                                  // 28
    uint64_t ist1, ist2, ist3, ist4, ist5, ist6, ist7;  // 36..91  Interrupt Stack Table
    uint64_t reserved2;                                  // 92
    uint16_t reserved3;                                  // 100
    uint16_t iomap_base;                                 // 102 (== sizeof(Tss64) => no bitmap)
} __attribute__((packed));

}  // namespace kernel
```

- [ ] **Step 3: Utworzyć `arch/x86_64/cpu/Gdt64.h`**

```cpp
/*
 * Gdt64.h — x86-64 Global Descriptor Table.
 *
 * Code/data descriptors keep the legacy 8-byte shape, but in long mode the base and limit
 * are ignored — only the access byte and the L (long-mode) flag matter. The TSS descriptor
 * is the one that grows: a 16-byte system descriptor carrying a full 64-bit base.
 *
 * Selectors (must match the IDT gates and the CS reload in initialize):
 *   0x08 kernel code64, 0x10 kernel data64, 0x18 user code64 (DPL3),
 *   0x20 user data64 (DPL3), 0x28 TSS (16-byte descriptor, indices 5..6).
 */
#pragma once
#include "Tss64.h"
#include <stdint.h>

namespace kernel {

// 8-byte code/data descriptor. flags high nibble = G/D/L/AVL; low nibble = limit 19:16.
struct GdtEntry64 {
    uint16_t limit_lo;
    uint16_t base_lo;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  flags;
    uint8_t  base_hi;
} __attribute__((packed));

// 16-byte TSS system descriptor: the base is scattered across 64 bits.
struct TssDescriptor64 {
    uint16_t limit_lo;
    uint16_t base_lo;
    uint8_t  base_mid1;
    uint8_t  access;        // 0x89 = present, DPL0, type 9 (available 64-bit TSS)
    uint8_t  flags;         // limit 19:16 + granularity (0 here; TSS is small)
    uint8_t  base_mid2;
    uint32_t base_hi;       // base bits 63:32
    uint32_t reserved;
} __attribute__((packed));

struct Gdt64Ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

class Gdt64 {
    struct __attribute__((packed)) Table {
        GdtEntry64      null_;     // 0x00
        GdtEntry64      kcode;     // 0x08
        GdtEntry64      kdata;     // 0x10
        GdtEntry64      ucode;     // 0x18 (DPL3, used from Plan 6)
        GdtEntry64      udata;     // 0x20 (DPL3, used from Plan 6)
        TssDescriptor64 tss;       // 0x28 (16 bytes, occupies two 8-byte slots)
    } table;
    Gdt64Ptr ptr;
public:
    void initialize();                  // build table, lgdt, reload CS/segs, point TSS at IST1
    void setKernelStack(uint64_t rsp0); // TSS.rsp0 — kernel stack for ring3->ring0 (Plan 6)
    void loadTss();                     // ltr 0x28 (after initialize)
private:
    static void setCodeData(GdtEntry64& e, uint8_t access, uint8_t flags);
    void setTss(uint64_t base, uint32_t limit);
};

}  // namespace kernel
```

- [ ] **Step 4: Utworzyć `arch/x86_64/cpu/Gdt64.cpp`**

```cpp
#include "Gdt64.h"
#include <string.h>   // memset

namespace kernel {

// The TSS and its IST1 (double-fault) stack live for the kernel's lifetime; the descriptor
// holds a pointer into g_tss64. File scope (.bss, trivial init) so no global ctor is needed.
static Tss64  g_tss64 __attribute__((aligned(16)));
static uint8_t g_dfStack[4096] __attribute__((aligned(16)));   // IST1: dedicated #DF stack

void Gdt64::setCodeData(GdtEntry64& e, uint8_t access, uint8_t flags) {
    // base/limit are ignored by the CPU in long mode; zero them. access carries
    // present/DPL/type; flags carries the L (long-mode) bit for code segments.
    e.limit_lo = 0; e.base_lo = 0; e.base_mid = 0; e.base_hi = 0;
    e.access = access;
    e.flags = flags;
}

void Gdt64::setTss(uint64_t base, uint32_t limit) {
    TssDescriptor64& t = table.tss;
    memset(&t, 0, sizeof(t));
    t.limit_lo  = (uint16_t) (limit & 0xFFFF);
    t.base_lo   = (uint16_t) (base & 0xFFFF);
    t.base_mid1 = (uint8_t)  ((base >> 16) & 0xFF);
    t.access    = 0x89;                                   // present | type=9 (avail 64-bit TSS)
    t.flags     = (uint8_t)  ((limit >> 16) & 0x0F);      // limit 19:16, G=0
    t.base_mid2 = (uint8_t)  ((base >> 24) & 0xFF);
    t.base_hi   = (uint32_t) ((base >> 32) & 0xFFFFFFFF);
    t.reserved  = 0;
}

void Gdt64::initialize() {
    memset(&table, 0, sizeof(table));

    // Code: present, ring/DPL, exec/read. flags = G | L (0xA0): L=1 marks a 64-bit code
    // segment (D/B must be 0). Data: present, write. Data segments ignore the L bit.
    setCodeData(table.kcode, 0x9A, 0xA0);   // 0x08 ring-0 code64
    setCodeData(table.kdata, 0x92, 0x00);   // 0x10 ring-0 data64
    setCodeData(table.ucode, 0xFA, 0xA0);   // 0x18 ring-3 code64 (DPL=3)
    setCodeData(table.udata, 0xF2, 0x00);   // 0x20 ring-3 data64 (DPL=3)

    // TSS @ 0x28. Point IST1 at a dedicated stack so #DF always lands on solid ground; mark
    // "no I/O bitmap". rsp0 is filled by setKernelStack before the first ring3->ring0 trap.
    g_tss64.ist1 = (uint64_t) (g_dfStack + sizeof(g_dfStack));
    g_tss64.iomap_base = sizeof(Tss64);
    setTss((uint64_t) &g_tss64, sizeof(Tss64) - 1);

    ptr.limit = sizeof(table) - 1;
    ptr.base  = (uint64_t) &table;

    // lgdt, then reload CS via a far return (long mode has no far-jmp-to-immediate from C):
    // push the new code selector + a RIP, lretq pops both. Then reload the data segment regs
    // with the kernel data selector (their bases are forced to 0 in long mode regardless).
    __asm__ __volatile__(
        "lgdt %0\n"
        "pushq $0x08\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%ss\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        : : "m"(ptr) : "rax", "memory");
}

void Gdt64::setKernelStack(uint64_t rsp0) { g_tss64.rsp0 = rsp0; }

void Gdt64::loadTss() { __asm__ __volatile__("ltr %0" : : "r"((uint16_t) 0x28)); }

}  // namespace kernel
```

- [ ] **Step 5: Zbudować obiekt (weryfikacja flag x86_64 kernela)**

```bash
docker run --rm -v "$(pwd)":/src -w /src nanos-build \
  x86_64-elf-g++ -c -ffreestanding -nostdlib -nostdinc++ --no-exceptions --no-rtti \
  -fno-leading-underscore -mno-red-zone -mno-sse -mno-mmx -mno-80387 \
  -Iarch/x86_64/cpu -Iinclude arch/x86_64/cpu/Gdt64.cpp -o /tmp/Gdt64.o && echo OK
```
Expected: `OK` (kompiluje się freestanding; `<string.h>` = `include/string.h`).

- [ ] **Step 6: Commit**

```bash
git add arch/x86_64/cpu/Port64.h arch/x86_64/cpu/Tss64.h arch/x86_64/cpu/Gdt64.h arch/x86_64/cpu/Gdt64.cpp
git commit -m "x86_64: add 64-bit GDT + TSS (16-byte TSS descriptor, IST1 for #DF)"
```

---

## Task 2: 16-bajtowe IDT (`Idt64.{h,cpp}`)

Gat IDT w long mode ma **16 bajtów**: 64-bitowa baza (`base_lo`/`base_mid`/`base_hi`) +
3-bitowe pole **IST**. 256 wektorów, PIC zremapowany na 0x20/0x28 jak w i686.

**Files:**
- Create: `arch/x86_64/cpu/Idt64.h`
- Create: `arch/x86_64/cpu/Idt64.cpp`

- [ ] **Step 1: Utworzyć `arch/x86_64/cpu/Idt64.h`**

```cpp
/*
 * Idt64.h — x86-64 Interrupt Descriptor Table.
 *
 * Each gate is 16 bytes: a 64-bit handler base split into lo/mid/hi, plus a 3-bit IST index
 * (0 = use the stack the CPU would normally pick; 1..7 = switch to TSS.istN). flags = type +
 * DPL + present (0x8E = present, DPL0, 64-bit interrupt gate, which also clears IF on entry).
 */
#pragma once
#include <stdint.h>

namespace kernel {

struct IdtEntry64 {
    uint16_t base_lo;     // handler bits 0:15
    uint16_t sel;         // code selector (0x08)
    uint8_t  ist;         // bits 0:2 = IST index, bits 3:7 = 0
    uint8_t  flags;       // P | DPL | type
    uint16_t base_mid;    // handler bits 16:31
    uint32_t base_hi;     // handler bits 32:63
    uint32_t reserved;
} __attribute__((packed));

struct Idt64Ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

class Idt64 {
    IdtEntry64 entries[256];
    Idt64Ptr   ptr;
public:
    void initialize();    // remap PIC, install all gates, lidt
private:
    void setGate(unsigned char num, uint64_t handler, uint16_t sel, uint8_t ist, uint8_t flags);
};

}  // namespace kernel

// CPU-exception stubs (isr64.S) and device-IRQ stubs (irq64.S). The int 0x80 / syscall gate
// is deliberately absent — userspace + syscalls arrive in Plan 6.
extern "C" void isr0();  extern "C" void isr1();  extern "C" void isr2();  extern "C" void isr3();
extern "C" void isr4();  extern "C" void isr5();  extern "C" void isr6();  extern "C" void isr7();
extern "C" void isr8();  extern "C" void isr9();  extern "C" void isr10(); extern "C" void isr11();
extern "C" void isr12(); extern "C" void isr13(); extern "C" void isr14(); extern "C" void isr15();
extern "C" void isr16(); extern "C" void isr17(); extern "C" void isr18(); extern "C" void isr19();
extern "C" void isr20(); extern "C" void isr21(); extern "C" void isr22(); extern "C" void isr23();
extern "C" void isr24(); extern "C" void isr25(); extern "C" void isr26(); extern "C" void isr27();
extern "C" void isr28(); extern "C" void isr29(); extern "C" void isr30(); extern "C" void isr31();
extern "C" void irq0();  extern "C" void irq1();  extern "C" void irq2();  extern "C" void irq3();
extern "C" void irq4();  extern "C" void irq5();  extern "C" void irq6();  extern "C" void irq7();
extern "C" void irq8();  extern "C" void irq9();  extern "C" void irq10(); extern "C" void irq11();
extern "C" void irq12(); extern "C" void irq13(); extern "C" void irq14(); extern "C" void irq15();
```

- [ ] **Step 2: Utworzyć `arch/x86_64/cpu/Idt64.cpp`**

```cpp
#include "Idt64.h"
#include "Port64.h"
#include <string.h>

namespace kernel {

void Idt64::setGate(unsigned char num, uint64_t handler, uint16_t sel, uint8_t ist, uint8_t flags) {
    IdtEntry64& e = entries[num];
    e.base_lo  = (uint16_t) (handler & 0xFFFF);
    e.base_mid = (uint16_t) ((handler >> 16) & 0xFFFF);
    e.base_hi  = (uint32_t) ((handler >> 32) & 0xFFFFFFFF);
    e.sel      = sel;
    e.ist      = (uint8_t) (ist & 0x07);
    e.flags    = flags;
    e.reserved = 0;
}

void Idt64::initialize() {
    memset(&entries, 0, sizeof(entries));

    // Remap the PIC to 0x20 (master) / 0x28 (slave), identical to i686, so hardware IRQs land
    // on vectors 32..47 instead of clashing with CPU exceptions 0..31.
    port_outb(0x20, 0x11); port_outb(0xA0, 0x11);
    port_outb(0x21, 0x20); port_outb(0xA1, 0x28);
    port_outb(0x21, 0x04); port_outb(0xA1, 0x02);
    port_outb(0x21, 0x01); port_outb(0xA1, 0x01);
    port_outb(0x21, 0x00); port_outb(0xA1, 0x00);

    // CPU exceptions 0..31. 0x8E = present, DPL0, 64-bit interrupt gate. #DF (vector 8) uses
    // IST1 (the dedicated stack wired in Gdt64) so a corrupt rsp can't cascade to a triple
    // fault; the rest use IST0 (#PF reports fine via CR2 without a separate stack).
    setGate(0,  (uint64_t) isr0,  0x08, 0, 0x8E);
    setGate(1,  (uint64_t) isr1,  0x08, 0, 0x8E);
    setGate(2,  (uint64_t) isr2,  0x08, 0, 0x8E);
    setGate(3,  (uint64_t) isr3,  0x08, 0, 0x8E);
    setGate(4,  (uint64_t) isr4,  0x08, 0, 0x8E);
    setGate(5,  (uint64_t) isr5,  0x08, 0, 0x8E);
    setGate(6,  (uint64_t) isr6,  0x08, 0, 0x8E);
    setGate(7,  (uint64_t) isr7,  0x08, 0, 0x8E);
    setGate(8,  (uint64_t) isr8,  0x08, 1, 0x8E);   // #DF -> IST1
    setGate(9,  (uint64_t) isr9,  0x08, 0, 0x8E);
    setGate(10, (uint64_t) isr10, 0x08, 0, 0x8E);
    setGate(11, (uint64_t) isr11, 0x08, 0, 0x8E);
    setGate(12, (uint64_t) isr12, 0x08, 0, 0x8E);
    setGate(13, (uint64_t) isr13, 0x08, 0, 0x8E);   // #GP
    setGate(14, (uint64_t) isr14, 0x08, 0, 0x8E);   // #PF
    setGate(15, (uint64_t) isr15, 0x08, 0, 0x8E);
    setGate(16, (uint64_t) isr16, 0x08, 0, 0x8E);
    setGate(17, (uint64_t) isr17, 0x08, 0, 0x8E);
    setGate(18, (uint64_t) isr18, 0x08, 0, 0x8E);
    setGate(19, (uint64_t) isr19, 0x08, 0, 0x8E);
    setGate(20, (uint64_t) isr20, 0x08, 0, 0x8E);
    setGate(21, (uint64_t) isr21, 0x08, 0, 0x8E);
    setGate(22, (uint64_t) isr22, 0x08, 0, 0x8E);
    setGate(23, (uint64_t) isr23, 0x08, 0, 0x8E);
    setGate(24, (uint64_t) isr24, 0x08, 0, 0x8E);
    setGate(25, (uint64_t) isr25, 0x08, 0, 0x8E);
    setGate(26, (uint64_t) isr26, 0x08, 0, 0x8E);
    setGate(27, (uint64_t) isr27, 0x08, 0, 0x8E);
    setGate(28, (uint64_t) isr28, 0x08, 0, 0x8E);
    setGate(29, (uint64_t) isr29, 0x08, 0, 0x8E);
    setGate(30, (uint64_t) isr30, 0x08, 0, 0x8E);
    setGate(31, (uint64_t) isr31, 0x08, 0, 0x8E);

    // Hardware IRQs 0..15 -> vectors 32..47.
    setGate(32, (uint64_t) irq0,  0x08, 0, 0x8E);
    setGate(33, (uint64_t) irq1,  0x08, 0, 0x8E);
    setGate(34, (uint64_t) irq2,  0x08, 0, 0x8E);
    setGate(35, (uint64_t) irq3,  0x08, 0, 0x8E);
    setGate(36, (uint64_t) irq4,  0x08, 0, 0x8E);
    setGate(37, (uint64_t) irq5,  0x08, 0, 0x8E);
    setGate(38, (uint64_t) irq6,  0x08, 0, 0x8E);
    setGate(39, (uint64_t) irq7,  0x08, 0, 0x8E);
    setGate(40, (uint64_t) irq8,  0x08, 0, 0x8E);
    setGate(41, (uint64_t) irq9,  0x08, 0, 0x8E);
    setGate(42, (uint64_t) irq10, 0x08, 0, 0x8E);
    setGate(43, (uint64_t) irq11, 0x08, 0, 0x8E);
    setGate(44, (uint64_t) irq12, 0x08, 0, 0x8E);
    setGate(45, (uint64_t) irq13, 0x08, 0, 0x8E);
    setGate(46, (uint64_t) irq14, 0x08, 0, 0x8E);
    setGate(47, (uint64_t) irq15, 0x08, 0, 0x8E);

    ptr.limit = sizeof(entries) - 1;
    ptr.base  = (uint64_t) &entries;
    __asm__ __volatile__("lidt %0" : : "m"(ptr));
}

}  // namespace kernel
```

- [ ] **Step 3: Zbudować obiekt (weryfikacja składni)**

```bash
docker run --rm -v "$(pwd)":/src -w /src nanos-build \
  x86_64-elf-g++ -c -ffreestanding -nostdlib -nostdinc++ --no-exceptions --no-rtti \
  -fno-leading-underscore -mno-red-zone -mno-sse -mno-mmx -mno-80387 \
  -Iarch/x86_64/cpu -Iinclude arch/x86_64/cpu/Idt64.cpp -o /tmp/Idt64.o && echo OK
```
Expected: `OK`.

- [ ] **Step 4: Commit**

```bash
git add arch/x86_64/cpu/Idt64.h arch/x86_64/cpu/Idt64.cpp
git commit -m "x86_64: add 16-byte IDT (3-bit IST field) + PIC remap"
```

---

## Task 3: TrapFrame `Registers` + tablica handlerów (`Interrupt64.{h,cpp}`)

Brak `pusha`/`popa` w 64-bit — `Registers` opisuje **ręczny** układ stosu: 15 GPR
zapisanych przez stub, `int_no`/`err`, oraz `rip/cs/rflags/rsp/ss` wepchnięte przez CPU.
Handler dostaje **wskaźnik** do ramki (nie kopię jak i686), więc modyfikacja `*regs`
(np. przyszły zwrot z syscalla w Planie 6) propaguje się przy odtworzeniu.

**Files:**
- Create: `arch/x86_64/cpu/Interrupt64.h`
- Create: `arch/x86_64/cpu/Interrupt64.cpp`

- [ ] **Step 1: Utworzyć `arch/x86_64/cpu/Interrupt64.h`**

```cpp
/*
 * Interrupt64.h — x86-64 trap frame + interrupt handler registry.
 *
 * Registers MUST match the stack layout built by isr64.S/irq64.S exactly: the common stub
 * saves the 15 GPRs in an order such that the LAST-pushed register (r15) is at the LOWEST
 * address, i.e. the field declared FIRST here. Below the GPRs sit int_no/err_code (pushed by
 * the per-vector entry / CPU) and then the CPU's iretq frame (rip/cs/rflags/rsp/ss). The
 * struct is named Registers to mirror i686 so MI's opaque arch::TrapFrame stays portable.
 */
#pragma once
#include <stdint.h>

namespace kernel {

#define IRQ0 32
#define IRQ1 33
#define IRQ2 34
#define IRQ3 35
#define IRQ4 36
#define IRQ5 37
#define IRQ6 38
#define IRQ7 39
#define IRQ8 40
#define IRQ9 41
#define IRQ10 42
#define IRQ11 43
#define IRQ12 44
#define IRQ13 45
#define IRQ14 46
#define IRQ15 47

struct Registers {
    // 15 GPRs saved by the stub (r15 pushed last -> lowest address -> declared first).
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    // Pushed by the per-vector entry (int_no) and CPU/stub (err_code).
    uint64_t int_no, err_code;
    // Pushed automatically by the CPU on the interrupt (rsp/ss always pushed in long mode).
    uint64_t rip, cs, rflags, rsp, ss;
};

typedef void (*IsrHandler)(Registers*);

class Interrupt {
public:
    static void registerInterruptHandler(unsigned char n, IsrHandler handler);
};

}  // namespace kernel
```

- [ ] **Step 2: Utworzyć `arch/x86_64/cpu/Interrupt64.cpp`**

```cpp
/*
 * Interrupt64.cpp — dispatch from the asm stubs to the C handler table.
 *
 * isr_handler / irq_handler take a Registers* (a pointer to the on-stack frame), NOT a copy:
 * in 64-bit there is no popa, so the stub restores registers from the very frame the handler
 * may have edited. irq_handler also acknowledges the PIC (EOI) before dispatching.
 */
#include "Interrupt64.h"
#include "Port64.h"

namespace kernel {
static IsrHandler interruptHandlers[256];

void Interrupt::registerInterruptHandler(unsigned char n, IsrHandler handler) {
    interruptHandlers[n] = handler;
}
}  // namespace kernel

// Called from isr64.S (CPU exceptions 0..31).
extern "C" void isr_handler(kernel::Registers* regs) {
    if (kernel::interruptHandlers[regs->int_no] != 0)
        kernel::interruptHandlers[regs->int_no](regs);
}

// Called from irq64.S (device IRQs, vectors 32..47).
extern "C" void irq_handler(kernel::Registers* regs) {
    // EOI: acknowledge the slave PIC first if the IRQ came from it (vector >= 40), then the
    // master — otherwise the PIC won't deliver further interrupts on that line.
    if (regs->int_no >= 40)
        kernel::port_outb(0xA0, 0x20);
    kernel::port_outb(0x20, 0x20);

    if (kernel::interruptHandlers[regs->int_no] != 0)
        kernel::interruptHandlers[regs->int_no](regs);
}
```

- [ ] **Step 3: Zbudować obiekt**

```bash
docker run --rm -v "$(pwd)":/src -w /src nanos-build \
  x86_64-elf-g++ -c -ffreestanding -nostdlib -nostdinc++ --no-exceptions --no-rtti \
  -fno-leading-underscore -mno-red-zone -mno-sse -mno-mmx -mno-80387 \
  -Iarch/x86_64/cpu arch/x86_64/cpu/Interrupt64.cpp -o /tmp/Interrupt64.o && echo OK
```
Expected: `OK`.

- [ ] **Step 4: Commit**

```bash
git add arch/x86_64/cpu/Interrupt64.h arch/x86_64/cpu/Interrupt64.cpp
git commit -m "x86_64: add Registers trap frame + interrupt handler dispatch"
```

---

## Task 4: Stuby asemblerowe `isr64.S` + `irq64.S`

Sedno Planu: ręczny zrzut 15 GPR, `swapgs` **tylko** na granicy ring3↔ring0 (test CPL z
zapisanego CS), poszanowanie **red zone** i 16-bajtowego wyrównania stosu pod `call`,
`iretq`.

> **Red zone (128 B) i wyrównanie — dlaczego ten kod jest poprawny.** (1) Kernel jest
> kompilowany z `-mno-red-zone` (KARCHFLAGS), więc funkcje C handlerów **nie** używają 128
> bajtów poniżej `rsp` — stub może bezpiecznie pchać tam ramkę. (2) Stub **wyłącznie**
> `push`uje (dekrement-potem-zapis) — nigdy nie zapisuje poniżej `rsp` przed jego
> obniżeniem, więc nie depcze po red zonie przerwanego kodu. (3) Przy pułapce z ringu 3 CPU
> sam przełącza się na świeży stos z `TSS.rsp0`, więc red zone przerwanego procesu user
> pozostaje nietknięta. (4) **Wyrównanie:** w long mode CPU wyrównuje `rsp` do 16 B przed
> wepchnięciem ramki przerwania. Ramka CPU = 5×8 = 40 B, `err+int_no` = 16 B, 15 GPR =
> 120 B → razem 176 B (podzielne przez 16), więc w momencie `call isr_handler` `rsp` jest
> 16-bajtowo wyrównane, zgodnie z System V (przed `call` `rsp%16==0`).

> **`swapgs`.** Wykonujemy `swapgs` **tylko** gdy zapisany CS ma CPL=3 (wejście z ringu 3),
> bo zamienia bazę `GS` user↔kernel; błędne wykonanie z ringu 0 = GP/korupcja. W Planie 4
> cały kod biegnie w ringu 0, więc test `test byte [...], 3` zawsze daje 0 i `swapgs` jest
> pomijany (poprawny no-op). `KERNEL_GS_BASE` i realne użycie `swapgs` aktywują się w
> Planie 6 (userland). Offset CS jest inny niż w i686 (ramka 64-bit) — liczony jawnie niżej.

**Files:**
- Create: `arch/x86_64/cpu/isr64.S`
- Create: `arch/x86_64/cpu/irq64.S`

- [ ] **Step 1: Utworzyć `arch/x86_64/cpu/isr64.S`**

```nasm
; arch/x86_64/cpu/isr64.S — x86-64 CPU-exception entry stubs (vectors 0..31) + dispatch.
; No pusha/popa in long mode: save the 15 GPRs by hand into a kernel::Registers frame, pass
; &frame in rdi to isr_handler, restore, iretq. swapgs is toggled only across ring3<->ring0
; (tested via the saved CS). The int 0x80 / syscall gate is intentionally NOT here (Plan 6).

bits 64

extern isr_handler

; No-error-code vector: push a dummy 0 so every frame has the same shape.
%macro ISR_NOERRCODE 1
  global isr%1
  isr%1:
    push qword 0          ; err_code (dummy)
    push qword %1         ; int_no
    jmp isr_common_stub
%endmacro

; Error-code vector: the CPU already pushed the error code; just push the vector number.
%macro ISR_ERRCODE 1
  global isr%1
  isr%1:
    push qword %1         ; int_no
    jmp isr_common_stub
%endmacro

ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE   8           ; #DF
ISR_NOERRCODE 9
ISR_ERRCODE   10          ; #TS
ISR_ERRCODE   11          ; #NP
ISR_ERRCODE   12          ; #SS
ISR_ERRCODE   13          ; #GP
ISR_ERRCODE   14          ; #PF
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_NOERRCODE 17
ISR_NOERRCODE 18
ISR_NOERRCODE 19
ISR_NOERRCODE 20
ISR_NOERRCODE 21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29
ISR_NOERRCODE 30
ISR_NOERRCODE 31

isr_common_stub:
    ; At this point the stack (from rsp) is: [int_no][err][rip][cs][rflags][rsp][ss].
    ; Saved CS is at rsp+24 -> swapgs only if we came from ring 3 (CPL bits set).
    test byte [rsp + 24], 3
    jz .from_kernel_in
    swapgs
.from_kernel_in:

    ; Manually save the 15 GPRs (r15 ends at the lowest address = first Registers field).
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp          ; arg0 = kernel::Registers* (rsp is 16-byte aligned; see plan note)
    cld                   ; System V: DF must be clear on entry to C
    call isr_handler

    ; Restore the 15 GPRs in reverse push order.
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; rsp now points at int_no again: saved CS is at rsp+24. Swap GS back if returning to
    ; ring 3 (mirror of the entry test) BEFORE dropping int_no/err and iretq.
    test byte [rsp + 24], 3
    jz .from_kernel_out
    swapgs
.from_kernel_out:

    add rsp, 16           ; discard int_no + err_code
    iretq
```

- [ ] **Step 2: Utworzyć `arch/x86_64/cpu/irq64.S`**

```nasm
; arch/x86_64/cpu/irq64.S — x86-64 hardware-IRQ entry stubs (vectors 32..47) + dispatch.
; Same manual-save / swapgs / iretq shape as isr64.S, but dispatches to irq_handler (which
; sends the PIC EOI). Scheduler-driven deferred preemption is NOT wired here (no scheduler in
; Plan 4); it returns in a later plan, mirroring i686 irq.S's schedPreempt hook.

bits 64

extern irq_handler

%macro IRQ 2
  global irq%1
  irq%1:
    push qword 0          ; err_code (IRQs carry none)
    push qword %2         ; int_no (remapped vector 32..47)
    jmp irq_common_stub
%endmacro

IRQ 0,  32
IRQ 1,  33
IRQ 2,  34
IRQ 3,  35
IRQ 4,  36
IRQ 5,  37
IRQ 6,  38
IRQ 7,  39
IRQ 8,  40
IRQ 9,  41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

irq_common_stub:
    test byte [rsp + 24], 3
    jz .from_kernel_in
    swapgs
.from_kernel_in:

    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp          ; arg0 = kernel::Registers*
    cld
    call irq_handler

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    test byte [rsp + 24], 3
    jz .from_kernel_out
    swapgs
.from_kernel_out:

    add rsp, 16
    iretq
```

- [ ] **Step 3: Zasemblować oba (weryfikacja składni NASM elf64)**

```bash
docker run --rm -v "$(pwd)":/src -w /src nanos-build sh -c \
  'nasm -f elf64 arch/x86_64/cpu/isr64.S -o /tmp/isr64.o && \
   nasm -f elf64 arch/x86_64/cpu/irq64.S -o /tmp/irq64.o && echo OK'
```
Expected: `OK` (brak błędów asemblacji; symbole `isr0..31`, `irq0..15` globalne).

- [ ] **Step 4: Commit**

```bash
git add arch/x86_64/cpu/isr64.S arch/x86_64/cpu/irq64.S
git commit -m "x86_64: add isr/irq stubs (manual 15-GPR dump, swapgs, iretq, red-zone-safe)"
```

---

## Task 5: Impl `<arch/cpu.h>` (`cpu_x86_64.cpp`) + `<arch/irq.h>` (`irq_x86_64.cpp`)

`cpuInit` składa GDT→TSS→IDT→fault (kolejność jak i686: GDT pierwsze, bo gaty IDT używają
selektora 0x08 ważnego dopiero po naszym `lgdt`). **Bez `sti`** tutaj — przerwania włącza
`Kernel::start` po wpięciu handlerów (Task 7).

**Files:**
- Create: `arch/x86_64/cpu/cpu_x86_64.cpp`
- Create: `arch/x86_64/cpu/irq_x86_64.cpp`

- [ ] **Step 1: Utworzyć `arch/x86_64/cpu/cpu_x86_64.cpp`**

```cpp
/*
 * cpu_x86_64.cpp — x86-64 implementation of <arch/cpu.h>.
 *
 * Plan 4 scope: descriptor-table + interrupt-vector bring-up and the interrupt-flag
 * primitives. powerOff/cpuIdentify/rtcEpoch (CPUID/ACPI/CMOS — port semantics identical to
 * i686) are ported in a later plan when MI code that calls them is staged in; archLoadThreadTls
 * is a no-op until %fs.base / TLS arrives in Plan 6.
 */
#include <arch/cpu.h>
#include "Gdt64.h"
#include "Idt64.h"

namespace {
// GDT/IDT live for the kernel's lifetime (the CPU registers point into them). File scope
// (.bss, trivial ctor) so no global constructor is required (the loader calls kmain directly).
kernel::Gdt64 g_gdt;
kernel::Idt64 g_idt;

// Boot kernel stack for the first ring3->ring0 trap (before any scheduler). Once tasks exist
// they supply their own kernel stack via TSS.rsp0 on every switch.
unsigned char g_bootKstack[8192] __attribute__((aligned(16)));
}

namespace arch {

void faultInit();   // arch/x86_64/cpu/fault_x86_64.cpp — #GP/#PF debug handlers

void cpuInit() {
    // GDT first: the IDT gates reference code selector 0x08, valid only once we own the GDT.
    g_gdt.initialize();
    // Point TSS.rsp0 at the boot kernel stack and load the task register, so future
    // ring3->ring0 traps have a kernel stack to land on.
    g_gdt.setKernelStack((uint64_t) (g_bootKstack + sizeof(g_bootKstack)));
    g_gdt.loadTss();
    // IDT: remap the PIC and install all 256 gates (CPU exceptions + IRQs). No sti yet.
    g_idt.initialize();
    faultInit();
}

void cpuDisableInterrupts() { __asm__ __volatile__("cli"); }
void cpuEnableInterrupts()  { __asm__ __volatile__("sti"); }
void cpuHalt()              { __asm__ __volatile__("hlt"); }

// Save RFLAGS then disable interrupts; restore (re-enabling IF only if it had been set), so a
// critical section nests correctly regardless of the caller's interrupt state.
unsigned long cpuIrqSave() {
    unsigned long flags;
    __asm__ __volatile__("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}
void cpuIrqRestore(unsigned long flags) {
    __asm__ __volatile__("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
}

// TLS on x86-64 uses %fs.base, set up for user threads in Plan 6. No-op until then.
void archLoadThreadTls(unsigned /*base*/) { }

}  // namespace arch
```

- [ ] **Step 2: Utworzyć `arch/x86_64/cpu/irq_x86_64.cpp`**

```cpp
/*
 * irq_x86_64.cpp — x86-64 implementation of <arch/irq.h>.
 *
 * Maps arch-neutral IRQ lines to the remapped PIC vectors (IRQ_TIMER -> 32, IRQ_KEYBOARD ->
 * 33) and wraps the kernel::Interrupt handler table. The x86-64 TrapFrame is kernel::Registers;
 * the function-pointer cast is the MI/MD boundary (identical pattern to i686 irq_x86.cpp).
 */
#include <arch/irq.h>
#include "Interrupt64.h"

namespace arch {

void registerIrqHandler(unsigned irq, IrqHandler h) {
    kernel::Interrupt::registerInterruptHandler((unsigned char) (IRQ0 + irq), (kernel::IsrHandler) h);
}

void registerTrapHandler(unsigned vector, IrqHandler h) {
    kernel::Interrupt::registerInterruptHandler((unsigned char) vector, (kernel::IsrHandler) h);
}

}  // namespace arch
```

- [ ] **Step 3: Zbudować oba obiekty**

```bash
docker run --rm -v "$(pwd)":/src -w /src nanos-build sh -c '
 F="-c -ffreestanding -nostdlib -nostdinc++ --no-exceptions --no-rtti -fno-leading-underscore \
    -mno-red-zone -mno-sse -mno-mmx -mno-80387 -Iarch/include -Iarch/x86_64/cpu -Iinclude";
 x86_64-elf-g++ $F arch/x86_64/cpu/cpu_x86_64.cpp -o /tmp/cpu64.o && \
 x86_64-elf-g++ $F arch/x86_64/cpu/irq_x86_64.cpp -o /tmp/irq64c.o && echo OK'
```
Expected: `OK`.

- [ ] **Step 4: Commit**

```bash
git add arch/x86_64/cpu/cpu_x86_64.cpp arch/x86_64/cpu/irq_x86_64.cpp
git commit -m "x86_64: implement <arch/cpu.h> (cpuInit GDT/IDT/TSS) + <arch/irq.h>"
```

---

## Task 6: Handlery #GP/#PF (`fault_x86_64.cpp`)

Bez tych handlerów nieobsłużona pułapka kończy się milczącym triple-faultem (reset).
Drukują wektor/err/`rip`/`cs`/`rflags` (+ **CR2 64-bit** dla #PF) i zatrzymują CPU.
Wersja zabijająca proces user (jak i686 `killCurrentProcess`) dochodzi, gdy zostanie
zportowany scheduler/model procesów — tu cały kod biegnie w ringu 0.

**Files:**
- Create: `arch/x86_64/cpu/fault_x86_64.cpp`

- [ ] **Step 1: Utworzyć `arch/x86_64/cpu/fault_x86_64.cpp`**

```cpp
/*
 * fault_x86_64.cpp — x86-64 CPU-exception debug handlers (#GP vector 13, #PF vector 14).
 *
 * Print the fault details (64-bit rip/cr2 via Console::writeHex(unsigned long) from Plan 2)
 * and halt, so faults are debuggable instead of triple-faulting invisibly. readCr2() comes
 * from arch/x86_64/mm/PagingControl.h (Plan 3). Process-killing (ring-3 SIGSEGV) lands when
 * the scheduler is ported.
 */
#include "Interrupt64.h"
#include "PagingControl.h"   // kernel::readCr2 (64-bit)
#include "Console.h"

namespace {

void faultHandler(kernel::Registers* r) {
    kernel::Console::write("\n*** CPU EXCEPTION vec=");
    kernel::Console::writeHex((int) r->int_no);
    kernel::Console::write(" err=");
    kernel::Console::writeHex((unsigned long) r->err_code);
    kernel::Console::write(" rip=");
    kernel::Console::writeHex((unsigned long) r->rip);
    kernel::Console::write(" cs=");
    kernel::Console::writeHex((unsigned long) r->cs);
    kernel::Console::write(" rflags=");
    kernel::Console::writeHex((unsigned long) r->rflags);
    if (r->int_no == 14) {        // #PF: CR2 holds the faulting linear address
        kernel::Console::write(" cr2=");
        kernel::Console::writeHex((unsigned long) kernel::readCr2());
    }
    kernel::Console::write("\n    rax=");
    kernel::Console::writeHex((unsigned long) r->rax);
    kernel::Console::write(" rbx=");
    kernel::Console::writeHex((unsigned long) r->rbx);
    kernel::Console::write(" rsp=");
    kernel::Console::writeHex((unsigned long) r->rsp);
    kernel::Console::writeLine(" ***");
    // A kernel fault is unrecoverable: do NOT iret (it would re-fault on the same instruction).
    for (;;)
        __asm__ __volatile__("hlt");
}

}  // namespace

namespace arch {

void faultInit() {
    kernel::Interrupt::registerInterruptHandler(13, &faultHandler);   // #GP
    kernel::Interrupt::registerInterruptHandler(14, &faultHandler);   // #PF
}

}  // namespace arch
```

- [ ] **Step 2: Zbudować obiekt**

```bash
docker run --rm -v "$(pwd)":/src -w /src nanos-build \
  x86_64-elf-g++ -c -ffreestanding -nostdlib -nostdinc++ --no-exceptions --no-rtti \
  -fno-leading-underscore -mno-red-zone -mno-sse -mno-mmx -mno-80387 \
  -Iarch/include -Iarch/x86_64/cpu -Iarch/x86_64/mm -Idrivers -Iinclude \
  arch/x86_64/cpu/fault_x86_64.cpp -o /tmp/fault64.o && echo OK
```
Expected: `OK` (zależy od `PagingControl.h` z Planu 3 i `Console.h`).

- [ ] **Step 3: Commit**

```bash
git add arch/x86_64/cpu/fault_x86_64.cpp
git commit -m "x86_64: add #GP/#PF debug handlers (64-bit register + CR2 dump)"
```

---

## Task 7: Bring-up self-test (PIT IRQ0 + klawiatura IRQ1) + wpięcie w `Kernel::start`

Plan 4 dowodzi działania ścieżki przerwań **widocznie w QEMU**: PIT (IRQ0) tyka i co
sekundę drukuje kropkę; klawiatura (IRQ1) echo-uje scancode. (Pełny sterownik klawiatury
to kext z `/nanos/kext`; tu instalujemy minimalny handler bring-upowy — analogicznie do
bannera Planu 2.)

**Files:**
- Create: `arch/x86_64/cpu/irqtest64.cpp`
- Modify: `arch/x86_64/boot/KernelStage64.cpp` (plik z Planu 2)

- [ ] **Step 1: Utworzyć `arch/x86_64/cpu/irqtest64.cpp`**

```cpp
/*
 * irqtest64.cpp — Plan-4 bring-up self-test for the x86-64 interrupt path.
 *
 * Installs two device-IRQ handlers via the MI <arch/irq.h> registration (proving that path
 * works unchanged): IRQ0 (PIT @ ~100 Hz) prints one '.' per second as a heartbeat, and IRQ1
 * (PS/2 keyboard) echoes the make-code of each key press. This is the interrupt analogue of
 * Plan 2's banner; the real keyboard driver is a loadable kext (later).
 */
#include "Interrupt64.h"
#include "Port64.h"
#include "Console.h"
#include <arch/irq.h>

namespace {
volatile unsigned long g_ticks = 0;

// IRQ0: programmable interval timer. One dot per 100 ticks (~1 s) = visible proof it fires.
void pitTick(kernel::Registers*) {
    if (++g_ticks % 100 == 0)
        kernel::Console::write(".");
}

// IRQ1: read the scancode from the PS/2 data port. Echo only make-codes (bit 7 clear); break
// codes (key release) are ignored for a clean display.
void kbdEcho(kernel::Registers*) {
    unsigned char sc = kernel::port_inb(0x60);
    if (!(sc & 0x80)) {
        kernel::Console::write("[key sc=");
        kernel::Console::writeHex((int) sc);
        kernel::Console::write("]");
    }
}
}  // namespace

namespace kernel {

void irqSelfTest() {
    // PIT channel 0, mode 3 (square wave), ~100 Hz: divisor = 1193180 / 100.
    unsigned divisor = 1193180u / 100u;
    port_outb(0x43, 0x36);
    port_outb(0x40, (unsigned char) (divisor & 0xFF));
    port_outb(0x40, (unsigned char) ((divisor >> 8) & 0xFF));

    arch::registerIrqHandler(arch::IRQ_TIMER,    (arch::IrqHandler) pitTick);
    arch::registerIrqHandler(arch::IRQ_KEYBOARD, (arch::IrqHandler) kbdEcho);
}

}  // namespace kernel
```

- [ ] **Step 2: Wpiąć cpuInit + self-test + sti w staged `Kernel::start`**

W `arch/x86_64/boot/KernelStage64.cpp` (utworzony w Planie 2; Plan 3 dołożył tam
`arch::mmuInitKernel(...)`). Dołóż nagłówek i deklarację u góry pliku:

```cpp
#include <arch/cpu.h>             // arch::cpuInit / cpuEnableInterrupts / cpuHalt

namespace kernel { void irqSelfTest(); }   // arch/x86_64/cpu/irqtest64.cpp
```

W ciele `Kernel::start`, **po** wywołaniu `arch::mmuInitKernel(...)` (Plan 3) i **przed**
pętlą idle (banner `[ idle ]` z Planu 2), wstaw:

```cpp
    // Plan 4: real GDT/IDT/TSS + PIC remap, then install the bring-up IRQ handlers and
    // enable interrupts. After this the PIT heartbeat ('.') and keyboard echo are live.
    arch::cpuInit();
    kernel::irqSelfTest();
    arch::cpuEnableInterrupts();   // sti
    Console::writeLine("[ OK ] interrupts live: PIT IRQ0 heartbeat + keyboard IRQ1 echo");
```

Upewnij się, że końcowa pętla idle używa `arch::cpuHalt()` (`sti; hlt` nie jest potrzebne —
przerwania są już włączone), np.:

```cpp
    for (;;)
        arch::cpuHalt();
```

> Jeżeli Plan 2/3 zostawiły inną treść pętli idle — **nie duplikuj**, dołóż jedynie
> powyższe cztery wywołania w wskazanym miejscu i zostaw resztę bannera bez zmian.

- [ ] **Step 3: Zbudować `irqtest64.cpp`**

```bash
docker run --rm -v "$(pwd)":/src -w /src nanos-build \
  x86_64-elf-g++ -c -ffreestanding -nostdlib -nostdinc++ --no-exceptions --no-rtti \
  -fno-leading-underscore -mno-red-zone -mno-sse -mno-mmx -mno-80387 \
  -Iarch/include -Iarch/x86_64/cpu -Idrivers -Iinclude \
  arch/x86_64/cpu/irqtest64.cpp -o /tmp/irqtest64.o && echo OK
```
Expected: `OK`.

- [ ] **Step 4: Commit**

```bash
git add arch/x86_64/cpu/irqtest64.cpp arch/x86_64/boot/KernelStage64.cpp
git commit -m "x86_64: bring-up self-test (PIT IRQ0 heartbeat + keyboard IRQ1 echo) in staged start"
```

---

## Task 8: Wpięcie w build (staged + ARCH_SOURCES) + weryfikacja w QEMU + regresja i686

**Files:**
- Modify: `arch/x86_64/arch.mk` (`ARCH_SOURCES += ...`)
- Modify: `Makefile` (`STAGE64_OBJS += ...` + staged reguła `$(STAGE_BIN)%.o: %.S`)

- [ ] **Step 1: Dopisać obiekty cpu do `ARCH_SOURCES` (`arch/x86_64/arch.mk`)**

Plan 3 zostawił (`arch/x86_64/arch.mk:25` po jego edycji):
```makefile
ARCH_SOURCES=loader.o entry64.o AddressSpace.o mmu_x86_64.o
```
Dopisz obiekty cpu (źródła znajdzie `VPATH` z `arch/x86_64/cpu`; sufiks `64`/unikalne
basename'y eliminują kolizję z elf32 i686 w współdzielonym `bin/`):
```makefile
# Plan 4 adds the GDT/IDT/TSS + interrupt path. The 64-suffixed objects (Gdt64/Idt64/
# Interrupt64/isr64/irq64) avoid colliding with i686's same-named bin/*.o; cpu_x86_64/
# fault_x86_64/irq_x86_64/irqtest64 have unique basenames already.
ARCH_SOURCES=loader.o entry64.o AddressSpace.o mmu_x86_64.o \
             Gdt64.o Idt64.o Interrupt64.o isr64.o irq64.o \
             cpu_x86_64.o fault_x86_64.o irq_x86_64.o irqtest64.o
```

- [ ] **Step 2: Dopisać obiekty do `STAGE64_OBJS` + staged reguła NASM (`Makefile`)**

Plan 2 zdefiniował `STAGE64_OBJS` i reguły staged w sekcji kontenerowej. Zamień listę
`STAGE64_OBJS` (uwzględniając wpisy Planu 3, jeśli je dodał — `AddressSpace.o mmu_x86_64.o`):
```makefile
STAGE64_OBJS=loader64.o entry64.o console_x86_64.o bringup_stubs64.o bootinfo_x86_64.o \
             MultibootMmap.o kmain.o KernelStage64.o Console.o memory_manager.o Heap.o \
             string_funcs.o icxxabi.o AddressSpace.o mmu_x86_64.o \
             Gdt64.o Idt64.o Interrupt64.o isr64.o irq64.o \
             cpu_x86_64.o fault_x86_64.o irq_x86_64.o irqtest64.o
```
Dodaj staged regułę dla plików `.S` (NASM), obok istniejącej `$(STAGE_BIN)loader64.o:`
(basename'y `isr64`/`irq64` są unikalne, więc ogólny wzorzec wystarcza):
```makefile
# Staged NASM rule: isr64.S -> bin/stage64/isr64.o, irq64.S -> bin/stage64/irq64.o.
$(STAGE_BIN)%.o: %.S
	@mkdir -p $(STAGE_BIN)
	nasm -f $(ASM_FMT) $< -o $@
```

> Uwaga: jeśli Plan 3 dodał już `AddressSpace.o mmu_x86_64.o` do `STAGE64_OBJS`, **nie
> duplikuj** — dołóż tylko 9 obiektów cpu Planu 4.

- [ ] **Step 3: Zbudować staged obraz x86_64 w kontenerze**

```bash
docker run --rm -v "$(pwd)":/src -w /src nanos-build make ARCH=x86_64 _bringup64
ls -l bin/kernel64.bin bin/nanos64.iso
```
Expected: link `bin/stage64/*.o` → `bin/kernel64.bin` (elf64) bez nierozwiązanych symboli
(`isr_handler`/`irq_handler`/`isr0..31`/`irq0..15`/`faultInit`/`irqSelfTest` wszystkie
zdefiniowane); powstaje `bin/nanos64.iso`.

- [ ] **Step 4: Boot headless — heartbeat PIT + brak nieoczekiwanych pułapek**

```bash
qemu-system-x86_64 -cpu qemu64 -m 512 -cdrom bin/nanos64.iso \
  -display none -monitor unix:/tmp/qmon64,server,nowait \
  -no-reboot -d int -D /tmp/qlog64 &
QEMU_PID=$!
sleep 4
python3 - <<'PY'
import socket, time
s = socket.socket(socket.AF_UNIX); s.connect("/tmp/qmon64")
s.recv(4096)
s.sendall(b"screendump /tmp/x86_64-irq-a.ppm\n"); time.sleep(1)
PY
sips -s format png /tmp/x86_64-irq-a.ppm --out /tmp/x86_64-irq-a.png >/dev/null
echo "Open /tmp/x86_64-irq-a.png — expect the Plan-2 banner, '[ OK ] interrupts live ...',"
echo "and a row of dots growing (~1 dot/s) = PIT IRQ0 firing."
# No unexpected CPU faults: GP=0d, double=08, page=0e should NOT appear.
grep -E 'v=0d|v=08|v=0e' /tmp/qlog64 && echo "FAULT DETECTED (investigate)" || echo "no GP/DF/PF faults"
```
Expected: PNG pokazuje banner + linię „interrupts live" + rosnący ciąg kropek; `grep`
wypisuje „no GP/DF/PF faults". (Jeśli czarny ekran / `v=08`: zwykolejona ramka stosu lub
zła baza IDT — patrz układ `Registers` vs kolejność `push` w `isr64.S`.)

- [ ] **Step 5: Boot headless — echo klawiatury (IRQ1)**

```bash
qemu-system-x86_64 -cpu qemu64 -m 512 -cdrom bin/nanos64.iso \
  -display none -monitor unix:/tmp/qmon64b,server,nowait &
QEMU_PID=$!
sleep 3
python3 - <<'PY'
import socket, time
s = socket.socket(socket.AF_UNIX); s.connect("/tmp/qmon64b")
s.recv(4096)
for k in ("a","b","c"):
    s.sendall(("sendkey %s\n" % k).encode()); time.sleep(0.3)
s.sendall(b"screendump /tmp/x86_64-irq-kbd.ppm\n"); time.sleep(1)
PY
kill $QEMU_PID 2>/dev/null
sips -s format png /tmp/x86_64-irq-kbd.ppm --out /tmp/x86_64-irq-kbd.png >/dev/null
echo "Open /tmp/x86_64-irq-kbd.png — expect '[key sc=1e][key sc=30][key sc=2e]'"
echo "(scancodes for a/b/c) = keyboard IRQ1 path works."
```
Expected: PNG zawiera trzy wpisy `[key sc=..]` ze scancode'ami klawiszy a/b/c
(set-1 make-codes: a=0x1e, b=0x30, c=0x2e) — dowód ścieżki IRQ1.

- [ ] **Step 6: (Opcjonalnie) potwierdzić ścieżkę #PF — wstrzyknąć pułapkę**

Aby zweryfikować raport #PF/#GP, tymczasowo dodaj w `Kernel::start` (po `sti`) odczyt z
niemapowanego adresu niekanonicznego:
```cpp
    volatile unsigned long* bad = (volatile unsigned long*) 0xFFFF800000000000UL;
    (void) *bad;   // TEMP: triggers #PF -> faultHandler prints "*** CPU EXCEPTION vec=0e ... cr2=..."
```
Zbuduj i zbootuj jak w Step 4; PNG powinien pokazać linię `*** CPU EXCEPTION vec=0e ...
cr2=0xffff800000000000 ***`, a maszyna utknie na `hlt`. **Usuń ten kod po weryfikacji** i
przebuduj. (Krok diagnostyczny — nie commitować TEMP.)

- [ ] **Step 7: Regresja i686 + arch-clean**

Run: `make build && make check-arch`
Expected: kernel i686 buduje się (nowe pliki żyją wyłącznie w `arch/x86_64/`, sygnatury
`<arch/cpu.h>`/`<arch/irq.h>` niezmienione); `OK: MI layer is arch-clean.`

- [ ] **Step 8: Commit**

```bash
git add arch/x86_64/arch.mk Makefile
git commit -m "build: link x86_64 cpu/interrupt objects into staged bring-up"
```

- [ ] **Step 9: Zaktualizować spec — odhaczyć kamień 4**

W `docs/superpowers/specs/2026-06-15-x86_64-migration-analysis.md`, §7, dopisać przy
kamieniu 4 adnotację „✅ zrealizowane w plans/2026-06-15-x86_64-plan-4-interrupts.md".
(Spec bywa plikiem untracked — jeśli tak, odhaczenie zapisz tylko tutaj.)

```bash
git add docs/superpowers/specs/2026-06-15-x86_64-migration-analysis.md
git commit -m "docs: mark x86_64 milestone 4 (GDT/IDT/TSS + interrupts) done" || true
```

---

## Self-Review

- **Spec coverage:** realizuje §2.2 (Gdt/Idt 16-B/Tss 64-bit/Registers/isr/irq/cpu/fault) i
  kamień 4 z §7. Ryzyka §8 zaadresowane: **red zone** (kernel `-mno-red-zone`, stub tylko
  `push`uje, ring3 dostaje świeży `TSS.rsp0`), **swapgs** (tylko gdy CPL=3 z zapisanego CS;
  w Planie 4 zawsze no-op, aktywne w Planie 6), **adresy kanoniczne / CR2 64-bit** (#PF
  drukuje pełny `readCr2()`), **16-bajtowe deskryptory** (jawne bitfieldy `IdtEntry64`/
  `TssDescriptor64`). IST: `ist1` dla #DF zapięte (deskryptor TSS + gat IST=1); #PF/#GP na
  IST0 (CR2 wystarcza).
- **Placeholders:** brak — każdy krok ma pełny kod i oczekiwany wynik QEMU/Docker.
- **Naming consistency:** TrapFrame = `Registers` (`uint64_t`), zgodny z opaque
  `arch::TrapFrame`; obiekty kolidujące z i686 mają sufiks `64` (`Gdt64.o`/`Idt64.o`/
  `Interrupt64.o`/`isr64.o`/`irq64.o`) i są w `ARCH_SOURCES` + `STAGE64_OBJS`; impl-pliki
  `cpu_x86_64.cpp`/`irq_x86_64.cpp`/`fault_x86_64.cpp` zgodne z kontraktem. Kolejność
  `Registers` ↔ kolejność `push` w `isr64.S`/`irq64.S` jawnie zsynchronizowana (r15 ostatni
  push = pierwsze pole). `isr_handler`/`irq_handler` biorą `Registers*` (nie kopię — brak
  `popa` w 64-bit), więc edycja ramki propaguje się przy odtworzeniu (pod syscall w Planie 6).
- **Zakres świadomie odłożony:** gat `int 0x80`/`syscall` (Plan 6), `killCurrentProcess`
  przy pułapce z ringu 3 + deferred preempt w `irq64.S` (po porcie schedulera),
  `powerOff`/`cpuIdentify`/`rtcEpoch`/`archLoadThreadTls` pełne (gdy odpowiednie MI wejdzie
  do staged buildu / Plan 6/7).

## Zależności

- **Plan 2** (boot + konsola + MI kmain): dostarcza staged build (`bin/stage64/`,
  `STAGE64_OBJS`, reguły `_stage64`/`_bringup64`, GRUB ISO), `console_x86_64` (sink VGA),
  `Console::writeHex(unsigned long)` (zrzut 64-bit) oraz **`KernelStage64.cpp`** z okrojonym
  `Kernel::start`, w który Task 7 wpina `cpuInit`/`irqSelfTest`/`sti`.
- **Plan 3** (paging PML4): `mmuInitKernel` zbudował własny PML4 identity-mapujący RAM, więc
  IDT i handlery leżą w zmapowanej pamięci; dostarcza **`arch/x86_64/mm/PagingControl.h`**
  (`kernel::readCr2()` 64-bit) używany przez `fault_x86_64.cpp`. Wpięcie z Tasku 7 następuje
  **po** `arch::mmuInitKernel(...)`.
- **Wymaga dalej:** Plan 5 (storage) i Plan 6 (syscalle/userland — gat `syscall`,
  `swapgs` aktywny, `%fs.base`) budują na tej ścieżce przerwań.

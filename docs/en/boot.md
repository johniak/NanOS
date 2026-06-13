# NanOS Boot Sequence

From power-on to the shell: GRUB2 loads the Multiboot kernel, `loader.s` sets up a stack and
calls `kmain`, and `Kernel::start` brings up the machine subsystem by subsystem before handing the
CPU to the scheduler, which runs `init` in ring 3. This doc traces that path. For what each
subsystem does once up, see the per-subsystem docs (memory.md, scheduler.md,
filesystem.md, terminal.md).

```
GRUB2 ──multiboot──► loader.s (0x100000) ──► kmain() ──► Kernel::start()
  reads /nanos/core/kernel.bin   save magic+mbd      MI entry      ordered bring-up ──► Scheduler::start()
  from the ext partition         set ESP, call kmain               (CPU→paging→FS→dev→…)      └─► init.nxe (ring 3)
```

---

## 1. GRUB2 + Multiboot

GRUB2 is installed in the disk image (`scripts/create-grub2-image.sh`): the MBR boot code + a
`grub-mkimage` core (with the `multiboot`, `part_msdos`, `ext2`, `biosdisk` modules) loads
`/boot/grub/grub.cfg`, which does `multiboot /nanos/core/kernel.bin` — GRUB reads the kernel off
the **ext** partition and jumps to it per the Multiboot protocol.

The kernel is a Multiboot binary because `arch/x86/boot/loader.s` puts a **Multiboot header** in a
`.multiboot` section that `arch/x86/linker.ld` places **first**, at load address `0x100000` (1 MiB),
so GRUB's header scan finds it. The header sets `magic 0x1BADB002` and flags `ALIGN | MEMINFO |
VIDEO` — the VIDEO flag **requests a linear framebuffer** (1024×768×32), which is how NanOS gets a
graphics console. On handoff GRUB passes `eax = 0x2BADB002` (the boot magic) and `ebx` = a pointer
to the `MultibootInfo` structure (memory map + framebuffer info).

---

## 2. The assembly entry (`arch/x86/boot/loader.s`, GNU as / AT&T)

`ENTRY(loader)`:

1. reserve a boot stack (`stack_bottom … stack_top`) and set `esp`;
2. save the Multiboot handoff: `magic` ← `eax`, `mbd` ← `ebx` (globals later read by the boot-info
   layer);
3. `call kmain` — and if it ever returns, `cli; hlt` forever.

It also provides the descriptor-load stubs the C++ side calls: `gdt_flush` (load GDTR, reload the
data segment registers to `0x10`, far-jump to reload `cs = 0x08`) and `idt_load` (load IDTR, `sti`).

---

## 3. Boot info: memory map + framebuffer

`arch/x86/boot/` parses what GRUB handed over and exposes it through the `<arch/bootinfo.h>`
contract (`bootinfo_x86.cpp`):

- **`MultibootInfo.h`** — the packed Multiboot struct (flags, `mmap_*`, `framebuffer_*`).
- **`MultibootMmap.cpp`** — walk the memory-map buffer (entry stride = `size + 4`), find the highest
  usable (`type == 1`) address.
- **`bootMemTop()`** — top of usable RAM (fallback 128 MiB, QEMU's default).
- **`bootMemForEachUsable(cb)`** — iterate usable ranges (feeds the frame allocator, memory.md).
- **`bootFramebuffer()`** — the linear RGB framebuffer (addr/pitch/w/h/bpp), or none → VGA text.

---

## 4. `kmain` → `Kernel::start`

`init/kmain.cpp` is the tiny MI entry: construct a `Kernel`, call `start()`. `Kernel::start`
(`kernel/Kernel.cpp`) is the ordered bring-up. Each step after the framebuffer comes up prints a
Linux-style `[ OK ]` line (`okBegin` writes `[    ] msg`, the step runs, `okEnd` rewrites the marker
green):

| # | Step | What |
|---|---|---|
| 1 | `arch::cpuInit()` | GDT + TSS, IDT + PIC remap, fault handlers (§5) |
| 2 | `setBootEpoch(arch::rtcEpoch())` | seed the wall clock from the RTC (file timestamps); seed the CSPRNG |
| 3 | `initPaging()` | frame allocator from the mmap, enable paging, map the framebuffer MMIO, **switch the console to fbcon** (memory.md, terminal.md) |
| — | boot splash | `NanOS -- booting` + `[ OK ]` for CPU, Paging, Framebuffer |
| 4 | storage stack | register `bootDisk()`, `new Vfs`, register ext2/ext4 types, mount synthetic `/`, mount ext at `/disks/main` (partition LBA from the MBR) |
| 5 | `extRwSelftest(vfs)` | write+read `/disks/main/nanos/rwtest`, report persistence (filesystem.md §5) |
| 6 | `/tmp`, `/etc` | mount RamFs tmpfs; populate `/etc` from the on-disk template |
| 7 | `/dev` nodes | `fb0` (if framebuffer), `input0` (keyboard evdev), `ptmx`/`pts0`/`tty` (PTY) — terminal.md |
| 8 | PCI | install the config-space backend + enumerate (finds the e1000) |
| 9 | `loadAllKexts("/disks/main/nanos/kext")` | load the PS/2 + e1000 drivers (kext.md) |
| 10 | `installSyscalls(vfs)` | wire `int 0x80` + a self-test (syscalls.md) |
| 11 | scheduler | `Scheduler::init()` (idle), create the `init` task (pid 1), register the idle + net kthreads, bring up the net, `archTimerInit(1000)` |
| 12 | `Scheduler::start()` | switch into the first task — never returns (scheduler.md) |

---

## 5. GDT / IDT / PIC — and the triple-fault fix

`arch::cpuInit()` (`cpu_x86.cpp`) installs **NanOS's own GDT** (`Gdt.cpp`), sets the TSS kernel
stack, loads the TSS, then initializes the IDT (`Idt.cpp`).

**The flat GDT** — six entries, each spanning 0–4 GiB:

| Selector | Entry | Use |
|---|---|---|
| `0x00` | null | — |
| `0x08` | ring-0 code | kernel; **the selector every IDT gate hardcodes** |
| `0x10` | ring-0 data | kernel |
| `0x1B` | ring-3 code (DPL 3) | userspace |
| `0x23` | ring-3 data (DPL 3) | userspace |
| `0x28` | TSS | ring3→ring0 stack switch (`esp0`) |

**The IDT** — 256 gates: 0–31 CPU exceptions, 32–47 the remapped hardware IRQs, and **gate 128
(`int 0x80`) with DPL 3** so ring-3 code can call it. `Idt::initialize` also **remaps the 8259 PIC**
master→`0x20`, slave→`0x28` (off the CPU-exception vectors), then `idt_load` + `sti`.

> **Why our own GDT — the original "GRUB2 doesn't boot" bug.** The IDT gates hardcode code selector
> `0x08`. GRUB2 hands control over with *its* GDT, where `0x08` is not the kernel code segment, so
> the first hardware IRQ vectored through a bad selector → #GP → #DF → **triple fault → reboot**.
> The fix is exactly the order above: install a flat GDT (so `0x08` is valid kernel code) **before**
> the IDT enables interrupts. This is bug #1 in CLAUDE.md and the reason `cpuInit` runs first.

---

## 6. Handoff to userspace (`init`, ring 3)

The scheduler's first task runs `initTaskBody` (`Kernel.cpp`): let the `[ OK ]` splash sit ~2 s
(yielding each tick), `clearScreen`, then `execProgram(vfs, "/disks/main/nanos/core/init.nxe")` —
which loads the `.nxe`, builds its address space, and `iret`s into **ring 3** (nxe-ndl.md §6;
this is the current trap-frame model — `init` is a normal ring-3 process, *not* ring 0). `init.nxe`
brings up networking via DHCP, starts the listening services, and `execve`s the login shell
(writing-apps.md §3.7). If the load fails, `initTaskBody` prints the error and returns.

---

## 7. Boot artifacts

`scripts/create-grub2-image.sh` (run inside the `nanos-build` container by `make image`) builds a
32 MiB raw disk: an MBR with one bootable partition at LBA 2048, an **ext4** filesystem on it,
`grub.cfg` written in, and GRUB2's `boot.img` (MBR) + `core.img` (sector 1) installed. The kernel
lives at `/nanos/core/kernel.bin` and the bootloader stages at `/boot/grub/` (filesystem.md
§2). `make iso`/`run-iso` wrap the same GRUB into a bootable ISO via `grub-mkrescue`.

**Key files:** `grub.cfg`, `arch/x86/boot/{loader.s, linker.ld, MultibootInfo.h, MultibootMmap.cpp,
bootinfo_x86.cpp}`, `init/kmain.cpp`, `kernel/Kernel.cpp`, `arch/x86/cpu/{cpu_x86.cpp, Gdt.cpp,
Idt.cpp}`, `scripts/create-grub2-image.sh`.

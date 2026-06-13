# NanOS Kernel Modules (kext / `.nkext`)

A **kext** ("nkext", Nano Kernel EXTension) is a device driver that is **not** compiled into
`kernel.bin` — it ships as a separate file under `/nanos/kext/` and the kernel **loads it at
boot**. It runs in ring 0, in the kernel's address space, with direct hardware access, but it
talks to the kernel only through a small, stable, **import-by-name** C ABI — the same
Windows-PE-style dynamic-link model NanOS uses for userland `.nxe`/`.ndl` (see
filesystem.md for where modules live, and networking.md for the e1000 kext in
context).

> Contrast with Linux: this is Linux's loadable-module idea (`insmod`/`modprobe`, `EXPORT_SYMBOL`,
> drivers as `.ko` outside `vmlinuz`) — but minimal: one fixed export list, load-at-boot only (no
> `modprobe`/dependency resolution, no `rmmod` yet), and the same binary format as userland.

---

## 1. Why kexts exist

- **Keep the kernel image small.** The PS/2 keyboard, PS/2 mouse, and e1000 NIC drivers are
  **not** in `kernel.bin` — they are three `.nkext` files. `kernel.bin` carries only the core
  (CPU/MM/VFS/net core/scheduler) and the module loader.
- **A driver is a shippable, identifiable unit.** Adding hardware support = dropping another
  `.nkext` into `/nanos/kext/`. The hooks the kernel already has (`DeviceManager`,
  `Vfs::registerType`, the net device registry, the IRQ table) are the registration points a
  module uses.
- **One binary format for everything.** Programs (`.nxe`), shared libraries (`.ndl`), and kernel
  modules (`.nkext`) are all **NxFormat** — same loader engine, same relocation, same
  import-by-name binding. A kext is essentially a `.ndl` whose imports resolve to the *kernel's*
  export table instead of `libc.ndl`.

---

## 2. The binary format

A `.nkext` is an **NxFormat** module (`kernel/NxFormat.h`, magic `"NXE"` = `0x0045584E`,
`NX_VERSION 3`). It is the Windows-PE-style model: linked at a *preferred* base but loadable
anywhere — the loader adds the load delta `(actualBase − preferredBase)` to every address in the
**relocation table** (R_386_32 absolute fixups) and patches each **import** by name.

The `NxHeader` fields a kext uses:

| Field | Meaning for a kext |
|---|---|
| `entry` | the module entry point — **`nkext_init`** (the `ENTRY` in `kext/kext.ld`). |
| `loadBase` | preferred base **`0xD0000000`** — arbitrary; everything is relocated at load. |
| `importTable` / `importCount` | `NxImport[]` — kernel symbols the module needs, scoped to library `"kernel"`. |
| `relocTable` / `relocCount` | `NxReloc[]` — R_386_32 fixups, applied with the load delta. |
| `bssStart` / `bssEnd` | zeroed by the loader. |
| `exportTable` / `exportCount` | unused by current kexts (a module *could* export, but none do). |

On-disk layout is the standard NxFormat one:
`[ NxHeader | code | rodata | data | NxImport[] | NxExport[] | NxReloc[] | NxNeeded[] | strings ]`
(`bss` is not stored).

---

## 3. Loading (boot time)

`kernel/Kernel.cpp` (`Kernel::start`) calls **`loadAllKexts(vfs, "/disks/main/nanos/kext")`**
after PCI enumeration (so a NIC driver can find its device) and before the syscall interface and
scheduler come up:

```
PCI bus enumeration → loadAllKexts(/nanos/kext) → syscall interface → scheduler/init
```

`kernel/KextLoader.cpp`:

- **`loadAllKexts(vfs, dir)`** — `readdir`s the directory, filters names ending in `.nkext`, and
  `loadKext`s each, printing `kext: <name> [loaded]` / `[FAILED]`. Returns the count loaded.
- **`loadKext(vfs, path)`** —
  1. `stat` + peek the `NxHeader` to size the buffer (must hold the stored image **and** its bss:
     `bssEnd − loadBase`, rounded up to a page).
  2. Allocate **page-aligned** kernel memory (`new` + align up to `0x1000`). It is identity-mapped
     and executable (i686, no NX bit), so the relocated code **runs in place at ring 0**.
  3. Read the file in, then `loadKextImage` → `NxeLoader::loadImage` with
     `delta = buffer − loadBase`, resolver = **`kernelResolveSym`** — applies relocations, binds
     imports, zeroes bss.
  4. Record the module in the resident registry `g_mods[]` (max **16**, `MAXKEXT`). The image
     buffer is **kept, never freed** — the foundation for a future `rmmod`/unload.
  5. Call the entry: `int (*)()` at `nkext_init`, **in ring 0**, which registers the module's IRQ
     handler and device(s).

**Failures are loud, never silent:** an import the kernel does not export makes `NxeLoader` return
`-2`, `loadKext` frees the buffer and reports `[FAILED]`.

The shared loader engine is `kernel/NxeLoader.cpp` (`loadImage`) — identical for `.nxe`, `.ndl`,
and `.nkext`; the only difference is the resolver/library passed in.

---

## 4. The kernel↔kext ABI

The kernel exports a fixed set of `extern "C"` functions to modules — NanOS's `EXPORT_SYMBOL`.
The **single source of truth** is `kernel/kexports.def`, an X-macro list (`KX(name)`); two
consumers expand it:

- `kernel/KernelExports.cpp` builds the runtime resolver table; **`kernelResolveSym(name, lib)`**
  looks a name up and returns its address (or 0 → the import fails to bind). All kernel symbols
  share the implicit `"kernel"` library namespace.
- the Makefile (awk) generates the kext **import stub** (`__imp_<name>` slots + thunks).

`kernelExportsInit(SynthFs* root)` is called once at boot (`Kernel.cpp`) so that
`knx_add_input_dev()` can register `/dev/input<N>` nodes on the synthetic root.

The exported symbols (`kexports.def`):

| Category | Symbols |
|---|---|
| **Memory** | `knx_malloc`, `knx_free` (kernel heap) |
| **Logging** | `knx_log(const char*)` (console) |
| **Time** | `knx_uptime_us()` |
| **Interrupts** | `knx_register_irq(int irq, void (*h)(void*))` |
| **Input** | `knx_feed_scancode(unsigned char)`, `knx_add_input_dev(CharDevice*)` → `/dev/input<N>` |
| **PCI** | `knx_pci_find`, `knx_pci_bar`, `knx_pci_bar_size`, `knx_pci_bar_is_io`, `knx_pci_irq`, `knx_pci_enable_bus_master`, `knx_pci_cfg_read32`, `knx_pci_cfg_write32` |
| **Net / DMA / MMIO** | `knx_map_mmio`, `knx_dma_alloc`, `knx_add_net_dev`, `knx_netif_rx` (the net ABI, `kernel/knx_net.h`) |

The networking ABI (`kernel/knx_net.h`) is deliberately **opaque**: a driver fills a flat
`KnxNetDev { name, mac, mtu, tx, drvctx }` and exchanges *raw frame bytes* — it never sees the
kernel's `NetDevice`/`NetBuf` internals, so the layout can't drift. `knx_netif_rx` is safe to call
from the driver's IRQ handler (it only enqueues + wakes the softirq thread; see
networking.md §2.2).

> Contrast with Linux: a single flat export list, no symbol versioning (`modversions`), no
> GPL-only symbols, no per-module namespaces. Imports are by name string, bound at load — not a
> passed vtable.

---

## 5. The modules that exist

All three load at boot from `/nanos/kext/` (`KEXTS = kbd mouse e1000` in the Makefile):

| Module | Source | Binds | Registers | Uses |
|---|---|---|---|---|
| **`kbd.nkext`** | `kext/kbd/kbd_ps2.cpp` | PS/2 8042, **IRQ1** | feeds scancodes to the kernel evdev/console | `knx_register_irq`, `knx_feed_scancode`, `knx_log` |
| **`mouse.nkext`** | `kext/mouse/mouse_ps2.cpp` (+ `MouseDevice.cpp`) | PS/2 AUX, **IRQ12** | a `CharDevice` → `/dev/input<N>` | `knx_register_irq`, `knx_add_input_dev`, … |
| **`e1000.nkext`** | `kext/e1000/e1000.cpp` | PCI Intel 82540EM (**`8086:100E`**) | a net device `eth0` (RX/TX DMA rings of 32) | `knx_pci_*`, `knx_map_mmio`, `knx_dma_alloc`, `knx_add_net_dev`, `knx_netif_rx` |

Each module's `nkext_init()` does the hardware bring-up and registration, then returns 0. The
keyboard line discipline and `/dev/input0` *policy* stay in the kernel — the kext owns only the
hardware (read a scancode on IRQ1, hand it over).

### Runtime registration hooks

| To register… | Call | Example |
|---|---|---|
| an IRQ handler | `knx_register_irq(irq, fn)` | `kbd`: `knx_register_irq(1, kbdIrq)` |
| an input device (`/dev/input<N>`) | `knx_add_input_dev(dev)` | `mouse` |
| a network device (`eth0`) | `knx_add_net_dev(&desc)` | `e1000` |
| find / configure a PCI device | `knx_pci_find` + `knx_pci_bar` + `knx_pci_enable_bus_master` | `e1000` |
| map device registers (MMIO) | `knx_map_mmio(phys, len)` | `e1000` BAR0 |
| allocate DMA memory | `knx_dma_alloc(len, &phys)` | `e1000` rings |

---

## 6. Building a kext

The kext build is **separate** from `kernel.bin` (kext objects are never in the kernel `SOURCES`):

- **Compile** (`KEXT_CFLAGS`): ring-0 freestanding C++ —
  `-ffreestanding -nostdlib -nostdinc++ --no-exceptions --no-rtti -fno-pic …`, includes
  `-Iarch/include -Ikernel -Idrivers -Iinclude`. Deliberately **no `-Ilib`** (the macOS
  case-insensitivity trap) and no picolibc.
- **Kernel import stub** `kimports.S` is generated from `kexports.def` by awk: an `__imp_<name>: dd
  0` slot per export in section `.nxlib.kernel`, plus a `<name>: jmp [__imp_<name>]` thunk in
  `.text`. `mknx` later tags those imports with source library `"kernel"`.
- **Runtime glue** `KEXT_GLUE = nxhdr.o + kimports.o + kext_rt.o`. `kext/kext_rt.cpp` is a tiny
  C/C++ runtime (`memset`/`memcpy`, `operator new`/`delete` delegating to `knx_malloc`/`knx_free`).
- **Link** with `kext/kext.ld` (`ENTRY(nkext_init)`, base `0xD0000000`, sections incl.
  `.nxheader`/`.nxlib.kernel`) using `-Wl,--emit-relocs` so the R_386_32 fixups survive.
- **`mknx`** (`tools/mknx.c`, a host tool) converts the linked ELF → `.nkext`: extracts sections,
  builds `NxReloc[]` from the relocations and `NxImport[]` from the `__imp_<name>` symbols.
- **Install**: the `_image` target `debugfs write`s each `bin/<m>.nkext` into `/nanos/kext/<m>.nkext`
  in the disk image, where `loadAllKexts` finds them.

### Adding a new kext

1. Write `kext/<name>/<name>.cpp` with `extern "C" int nkext_init()` and `extern "C"`
   declarations for the `knx_*` symbols you use.
2. Add a per-kext link rule in the Makefile (copy an existing one) and append `<name>` to `KEXTS`.
3. If you need a kernel facility that isn't exported yet, add a `KX(knx_…)` line to
   `kernel/kexports.def` and implement it in `kernel/KernelExports.cpp` (+ a declaration in the
   relevant `knx_*.h`). Both the resolver and the import stub regenerate from that one line.

---

## 7. Ring & privilege

A kext runs in **ring 0**, in the **kernel address space** (identity-mapped, executable — i686
has no NX bit), with **direct hardware access**: port I/O (inline `inb`/`outb`), MMIO, DMA, and
IRQ registration. There is **no isolation** from the kernel — a buggy kext can corrupt the system,
exactly like a Linux module.

> Contrast with userland `.nxe`: those run in **ring 3**, in an isolated address space, and reach
> the kernel only via `int 0x80` syscalls. A kext is the opposite end — full privilege, the same
> binary format.

---

## 8. Testing

`tests/test_kextloader.cpp` exercises the loader on the host (no QEMU): relocation with a non-zero
delta, import binding through a mock resolver, the `"kernel"` library scoping, and the loud-failure
path for an unresolved import. The three real modules are verified in QEMU (keyboard input, mouse
`/dev/input`, and `ping`/`wget` over the e1000 — see networking.md §12).

---

## 9. Limits & future work

| | Current |
|---|---|
| Resident modules | **16** (`MAXKEXT`) |
| Load | **at boot only**, all of `/nanos/kext/*.nkext` |
| Unload | **none yet** — module images are kept resident (the registry is the `rmmod` hook) |
| Dependencies | **none** — no `modprobe`-style dependency/auto-load |
| Exports from a kext | format supports it; no current module exports |
| Isolation | **none** (ring 0, shared address space) |

Future: `rmmod`/unload (the `g_mods[]` registry + kept buffers exist for it), on-demand load
(`modprobe` by device id), and more drivers (block devices, other NICs) over the same ABI —
adding a driver is adding a `.nkext`, not editing `kernel.bin`.

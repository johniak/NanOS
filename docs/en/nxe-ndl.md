# NanOS Executables & Shared Libraries (`.nxe` / `.ndl`)

NanOS does **not** run ELF at runtime. A program is compiled to ELF, then the host tool `mknx`
converts it to **NxFormat** — `.nxe` for a program, `.ndl` for a shared library. The link model is
**Windows-PE / MinGW style** (import-by-name, export-by-name, base relocations), not ELF/glibc.

This document is the **format + runtime-architecture reference**: the on-disk format, the loader
stack, dynamic linking, the userland memory map, and the ring-3 process lifecycle. For a **how-to**
(writing an in-tree app or porting a real Linux program with the nanos-sdk) see
writing-apps.md. The kernel-module variant `.nkext` is in kext.md; where the files live
on disk is filesystem.md.

---

## 1. One format, three variants

All three share `kernel/NxFormat.h` (magic `"NXE"` = `0x0045584E`, `NX_VERSION 3`) and the **same
loader engine** (`NxeLoader`). They differ only in how they're used and where imports resolve:

| Ext | What | Marker | Imports resolve against | Runs in |
|---|---|---|---|---|
| `.nxe` | program (executable) | — | loaded `.ndl` libraries | **ring 3** |
| `.ndl` | shared library | `NX_FLAG_DLL` | other `.ndl` libraries | ring 3 (in a process) |
| `.nkext` | kernel module | — | the kernel export table | **ring 0** (see kext.md) |

> Contrast with Linux: ELF + `ld.so` + GOT/PLT, symbols bound flat by `ELF hash`. NanOS is the
> Windows-PE model — a binary depends on a **stable named API** (`libc.ndl`), not a specific
> library version baked in. One binary runs on any build that exports that API: no "binary per
> distro" problem.

---

## 2. The format

On disk (and in memory when loaded at `loadBase`):

```
[ NxHeader | code | rodata | data | NxImport[] | NxExport[] | NxReloc[] | NxNeeded[] | strings | (bss — not stored) ]
```

All addresses in the header and tables are **absolute** (relative to `loadBase`). `NxHeader`
(`kernel/NxFormat.h`):

| Field | Meaning |
|---|---|
| `magic` / `version` / `flags` | `NX_MAGIC`, `NX_VERSION 3`, `NX_FLAG_DLL` (bit 0) for a library. |
| `entry` | program entry (`.nxe` → `_start`). |
| `loadBase` | preferred link base (`0x800000` for programs; libraries are relocated). |
| `imageSize` | bytes stored in the file (header + code + rodata + data + tables). |
| `bssStart` / `bssEnd` | zeroed by the loader, not stored. |
| `importTable` / `importCount` | `NxImport[]` — symbols to bind. |
| `exportTable` / `exportCount` | `NxExport[]` — symbols this module provides (libraries). |
| `relocTable` / `relocCount` | `NxReloc[]` — R_386_32 fixups. |
| `neededTable` / `neededCount` | `NxNeeded[]` — `.ndl` libraries to load first. |

The four table entry types:

- **`NxImport { nameOff, slotAddr, libOff }`** — resolve the symbol `nameOff` in the library named
  by `libOff` (per-DLL namespace; `libOff == 0` = resolve flat across all modules), then patch the
  IAT slot at `slotAddr` with the resolved address.
- **`NxExport { nameOff, addr }`** — the symbol `nameOff` is callable at `addr` once loaded.
- **`NxReloc { off }`** — the 32-bit word at `off` holds an absolute address; the loader adds the
  load delta `(actualBase − loadBase)`.
- **`NxNeeded { nameOff }`** — load the `.ndl` named `nameOff` before binding this module's imports.

---

## 3. The loader stack

Four layers, build-time to runtime:

```
mknx (build, host)      ELF -> NxFormat: extract sections, collect R_386_32 -> NxReloc[],
  │                     build NxImport/NxExport/NxNeeded[], pack the string pool.
  ▼
NxeLoader (kernel, MI)  ONE image: apply relocations(+delta), bind the IAT via a resolver
  │   NxeLoader.cpp     callback, zero bss, report exports. No I/O, host-tested.
  ▼
DynLoader (kernel)      DEPENDENCY GRAPH: recursively load NEEDED .ndl, dedup, allocate
  │   DynLoader.cpp     module windows, build per-DLL symbol tables, bind across modules.
  ▼
Exec + arch usermode    PROCESS: stage the image, create an AddressSpace, map image+stack,
      Exec.cpp          iret to ring 3. (execProgram / execve / fork / exit.)
```

### 3.1 `mknx` (`tools/mknx.c`)

A native host tool (`cc -O2 -Ikernel -o bin/mknx tools/mknx.c`). It reads an ELF32 (EM_386) linked
with `-Wl,--emit-relocs`, computes `loadBase`/`bss` extents, copies the `SHF_ALLOC PROGBITS`
sections into a flat image, turns the kept `R_386_32` relocations into `NxReloc[]`, builds the
import/export/needed tables, and writes the NxFormat file. Flags:

| Flag | Effect |
|---|---|
| `--dll` | set `NX_FLAG_DLL` (output is a library). |
| `--export NAME` / `--export-all` | put one named / all global+weak symbols into `NxExport[]`. |
| `--need NAME` | add a library to `NxNeeded[]` (the in-tree `%.nxe` rule auto-adds `--need libc.ndl`). |
| `--implib` + `--soname NAME` | emit an **import library** (one `.s` per export) instead of a module — see §5. |

### 3.2 `NxeLoader` (`kernel/NxeLoader.cpp`)

The pure, host-testable core — `loadImage(buf, cap, delta, resolve, entryOut, onExport, …)`:

1. **Relocate** — for each `NxReloc`, add `delta` to the 32-bit word.
2. **Bind imports** — for each `NxImport`, call `resolve(name, lib)`; on success patch `slotAddr`,
   on failure return **`-2`** (loud — never a silent unbound slot).
3. **Exports** — relocate each `NxExport.addr` and report it via `onExport` (how `DynLoader`
   harvests a library's symbols).
4. **Zero bss** (`bssStart..bssEnd`).

It does no I/O and no arch work, so it is the same engine for `.nxe`, `.ndl`, and `.nkext`; only
the resolver/library differ.

### 3.3 `DynLoader` (`kernel/DynLoader.cpp`) — dynamic linking

When a program (or library) has needed/imported symbols, `dynLoadProgram` resolves the whole graph:

- **Search path** is fixed: `/disks/main/nanos/lib/<name>.ndl`.
- **Recursive, post-order** (`ensureLib`): a module's `NEEDED` libraries are loaded **before** it,
  so a client of `libnwui.ndl` automatically gets `libnw.ndl` → `libc.ndl` (the dyld/Windows model).
- **Dedup by soname** (`LibSet`): a library shared across the graph (the diamond `libc.ndl`) loads
  **once** and is reused.
- **Load windows**: each `.ndl` gets a 4 MiB slot from the **module band** (`g_nextBase`, advancing
  by `arch::mmuModuleStride()` from `arch::mmuModuleBase()`), is run through `NxeLoader` with its
  relocation delta, then mapped into the process `AddressSpace` by `arch::archLoadModule`.
- **Per-DLL namespace**: every loaded library has its own `SymTable` (keyed by soname). An import
  with a named `libOff` resolves only against that library's table; `libOff == 0` resolves flat.
  `SymTable` is fixed-capacity (`MAX = 2048` symbols, `POOL = 32 KiB` of names — sized for
  picolibc's ~1300 exports), first definition wins.

---

## 4. Userland memory layout

Each process has its own `AddressSpace` (paging; see `arch/x86/mm/mmu_x86.cpp`). It shares the
kernel half (identity-mapped RAM, `0`–`0x8000000`) read-only via shared PDEs, and owns private page
tables for these windows (all verified against `mmu_x86.cpp` / `usermode_x86.cpp`):

| Region | VA range | Notes |
|---|---|---|
| **User window** | `0x800000`–`0xBFFFFF` | one 4 MiB PDE. Program image at `loadBase = 0x800000` (≈3.5 MiB max). |
| **User stack** | `0xB80000`–`0xC00000` | 512 KiB at the top of the user window; `esp` starts at `0xC00000`, grows down. |
| **Module band** (`.ndl`) | `0x08000000`–`0x10000000` | 4 MiB stride → **32** library slots (`NX_MOD_*`). |
| **Framebuffer** | `0x10000000` | user mapping of the FB (if present). |
| **Heap (`brk`)** | `0x20000000`–`0x22000000` | 32 MiB, grown by `SYS_brk`/`sbrk` (`NX_BRK_*`). |
| **mmap** | `0x30000000`–`0x34000000` | 64 MiB, bump-allocated for `mmap(MAP_ANONYMOUS)` + file-backed (`NX_MMAP_*`). |

**Staging window.** Before a process space exists, `execProgram`/`execve` read the `.nxe` into the
**staging window** `STAGE_BASE = 0x800000`, `STAGE_CAP = 4 MiB` (`kernel/Exec.cpp`) — the same VA
as the user window, but accessed under the **kernel** directory where `0x800000` is a reserved
identity-mapped frame. The image is relocated/zeroed there, then copied into the process's private
frames by `archLoadUser`. An image larger than `STAGE_CAP` is rejected before it can overrun.

> The program load base was raised `0x400000 → 0x800000` to give the kernel image headroom — the
> staging window, user window, and stack moved up in lockstep (`user/nx.ld`, `Exec.cpp`,
> `mmu_x86.cpp`, `usermode_x86.cpp` must all agree).

---

## 5. Linking against a library — import library + `nx-dllimport`

A program never links the `.ndl` itself; it links a small **import library** `lib<x>.ndl.a` built by
`mknx … --implib --export-all --soname <x>.ndl`. The archive has one member per export:

- **function** export → a thunk `name: jmp [__imp_name]` plus an IAT slot in a per-library section
  `.nxlib.<soname>`;
- **data** export → just the IAT slot.

The linker pulls only the members the program actually references. `user/nx.ld` keeps each
`.nxlib.<soname>` as a **separate output section** so the name survives linking — `mknx` reads it to
tag each `NxImport` with its source library (that is where `libOff` comes from). The slots live
inside the loaded image so the loader can patch them.

**Data imports** (`stdin`/`stdout`/`stderr`/`errno`/`environ`) can't be satisfied by a plain
extern across a module boundary, so `user/libc-glue/nx-dllimport.h` (force-included into program
sources, *not* into the glue that *is* libc) redefines each as a dereference of its IAT slot —
exactly the Windows `__declspec(dllimport)` model:

```c
extern int *__imp_errno;
#define errno (*__imp_errno)
```

The libraries that exist: **`libc.ndl`** (picolibc + `user/libc-glue` syscall layer, built
`--export-all`), **`libnw.ndl`** (NanWM compositor client), **`libnwui.ndl`** (UI toolkit, needs
libnw + libc), and the demo **`greet.ndl`**. Libraries are linked with `user/dll.ld` (preferred
base `0x09000000`, relocated at load).

---

## 6. The process lifecycle (ring 3)

### 6.1 Startup

`user/crt0.S` `_start` runs with `esp` pointing at `argc` (SysV i386): it derives `argv`/`envp`,
publishes the environment (`__nx_set_environ`) and program name (`__nx_set_progname`), calls
`main(argc, argv, envp)`, then `exit(main's return)`.

### 6.2 Entering ring 3

`kernel/Exec.cpp` `execProgram` (and the `execve(11)` syscall): stage the image, pick
`dynLoadProgram` (if it imports/needs anything) or `loadStaged` (delta 0, no imports), create a
fresh `AddressSpace`, then `arch::archLoadUser` maps the image and the 512 KiB stack into private
frames and builds the argv/envp image on the stack. Finally `arch::archEnterUser`
(`usermode_x86.cpp`) switches CR3 and **`iret`s to ring 3** — it does not return:

```
cli; mmuSwitch(space); ds=es=fs=gs = 0x23      ; user data, DPL 3
iret frame: ss=0x23, esp=userEsp, eflags=0x202 ; IF=1
            cs=0x1B, eip=entry                  ; user code, DPL 3
```

GDT selectors: **user code `0x1B`**, **user data `0x23`** (both DPL 3).

### 6.3 Syscalls and exit

A program calls the kernel via **`int 0x80`** (Linux i386 numbers, `kernel/SyscallNr.h`): `eax` =
number, `ebx/ecx/edx` = args, result in `eax` (negative = errno). The gate is IDT vector 128
(DPL 3). `exit(1)`/`exit_group` reaches `procExit` (`Exec.cpp`), which tears down the address space,
marks the task a zombie, and schedules away.

> This is the **Stage-4 trap-frame model**: ring 3 via `iret`, exit via syscall + scheduler. The
> earlier setjmp/longjmp-back-to-kernel scheme is gone (`usermode_x86.cpp`: "No longjmp").

### 6.4 fork / execve

`fork(2)` eagerly copies the address space (including the heap/module/mmap windows) and the fd
table; the child resumes from the same trap frame with `eax = 0`. `execve(2)` replaces the image
**in place**: stage + load into a fresh space, then `arch::archFrameToUser` rewrites the trapping
task's trap frame (`eip`=entry, `useresp`=userEsp, `cs/ss/ds`, `eflags`) so the syscall's tail
`iret` drops into the new program. The shell (`user/nsh.c`) resolves a command name to a path (see
filesystem.md) and `execve`s it; PID 1 (`init`) is started directly by the kernel via
`execProgram`.

### 6.5 Signals

Delivery runs in the target's own context (its user CR3 is active): `arch::archPushSignalFrame`
builds a `SigContext` plus the handler's argument and return address on the **user stack**, points
the trap frame at the handler, and lets the `iret` enter it in ring 3. The libc trampoline
(`user/libc-glue/sigtramp.*`) calls `sigreturn`, and `arch::archSigreturn` restores the saved
context — with the restart/EINTR/keep policy applied to the interrupted syscall.

---

## 7. Quick reference

| Constant | Value | Where |
|---|---|---|
| `NX_MAGIC` / `NX_VERSION` | `0x0045584E` / 3 | `kernel/NxFormat.h` |
| program `loadBase` | `0x800000` | `user/nx.ld`, `Exec.cpp STAGE_BASE` |
| library preferred base | `0x09000000` | `user/dll.ld` (relocated at load) |
| user window | `0x800000`–`0xBFFFFF` | `usermode_x86.cpp` |
| user stack | `0xB80000`–`0xC00000` (512 KiB) | `usermode_x86.cpp` |
| staging window | `0x800000`, 4 MiB | `Exec.cpp` |
| module band | `0x08000000`–`0x10000000`, 4 MiB stride (32) | `mmu_x86.cpp` |
| heap (`brk`) | `0x20000000`–`0x22000000` (32 MiB) | `mmu_x86.cpp` |
| mmap | `0x30000000`–`0x34000000` (64 MiB) | `mmu_x86.cpp` |
| user code / data selectors | `0x1B` / `0x23` (DPL 3) | `usermode_x86.cpp` |
| syscall gate | `int 0x80`, IDT 128 (DPL 3) | `SyscallNr.h`, `arch/x86/cpu/Idt.cpp` |
| `SymTable` capacity | 2048 symbols / 32 KiB names | `DynLoader.h` |

**Key files:** `kernel/NxFormat.h` (format), `tools/mknx.c` (ELF→Nx), `kernel/NxeLoader.cpp` (image
loader, host-tested), `kernel/DynLoader.cpp` (dynamic linker), `kernel/Exec.cpp` (process
lifecycle), `arch/x86/cpu/usermode_x86.cpp` (ring-3 entry + signals),
`arch/x86/mm/mmu_x86.cpp` (per-process address space), `user/{crt0.S,nx.ld,dll.ld,libc-glue/}`
(runtime). See writing-apps.md to actually build or port an app.

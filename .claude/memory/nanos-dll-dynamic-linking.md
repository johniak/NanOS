---
name: nanos-dll-dynamic-linking
description: "NanOS Windows-style .ndl dynamic-linking system — relocatable modules, import-by-name, per-DLL namespace, dllimport data symbols, libc.ndl; ALL userland dynamic incl. doom"
metadata: 
  node_type: memory
  type: project
  originSessionId: 661d85b4-63ba-4deb-8adc-c850a1d3125e
---

NanOS has a full Windows-PE-style dynamic-linking subsystem (branch `dockerized-build`, committed cdd0dc0 / 5e8efe5 / b04a5c7). Built to avoid the Linux "binary-per-distro" problem: a program depends on named symbols, not a baked-in libc.

- **Stage 1** — relocatable `.nxe`/`.ndl`: `kernel/NxFormat.h` v2 (NxReloc/NxExport/NxNeeded + DLL flag); `tools/mknx.c` (host C tool, ELF→module, replaces `objcopy -O binary`; link with `ld --emit-relocs`); loader applies R_386_32 base relocations by `loadDelta` (`kernel/NxeLoader.cpp`).
- **Stage 2** — `kernel/DynLoader.{h,cpp}`: loads needed `.ndl`s into a per-module 4 MiB window band at **0x08000000–0x10000000** (above RAM so each module PDE is private; teardown/fork in `arch/x86/mm/mmu_x86.cpp` cover it), builds a flat `SymTable`, binds imports by name. `arch::archLoadModule` maps a relocated module. Demo: `user/lib/greet.c` → greet.ndl, `user/usedll.c`.
- **Stage 3** — `libc.ndl`: picolibc + syscall/cwd/sigtramp glue, linked at preferred base 0x09000000, force-included via a curated `--undefined` list (NOT `--whole-archive`, which pulls picolibc's own sbrk/signal + getentropy/sigprocmask). `mknx --implib` generates the import library (a `name: jmp [__imp_name]` thunk + IAT slot per FUNC export). **nsh and free are migrated** to dynamically link libc.ndl; cat/ls/doom stay static.

- **Stage 4** (`5139f61`) — per-DLL namespace (format v3): `NxImport` gains `libOff`; each import records its source library and the loader resolves it against THAT library's table (PE model). Import-lib slots live in a per-lib section `.nxlib.<soname>` (kept as a SEPARATE output section in `user/nx.ld` — `*(.nxlib.*)` would MERGE and lose the name); mknx reads the section name → `libOff`. `mknx --soname`. `DynLoader` keeps a `soname→SymTable` map.
- **Stage 5** (`c315dda`) — **data-symbol imports** (Windows dllimport): mknx exports `STB_WEAK` too (stdin/out/err are weak). `user/libc-glue/dllimport.S` = `__imp_` slots; `user/libc-glue/nx-dllimport.h` = `#include`-then-`#undef`/`#define stdout (*__imp_stdout)` etc. (force-included only for PROGRAM objects via `$(DYNHDR)`, never glue). Migrated cat & ls.
- **Stage 6** (`e87332b`) — **ALL programs dynamic** incl. doom. Generic `%.nxe` rule is dynamic (`--need libc.ndl`); `DYN_GLUE`=crt0+nxhdr. Math (sin/cos/sqrt…) force-included into libc.ndl (libm.a is empty). doom links a trailing `-lc` FALLBACK that statically pulls only still-unresolved **const** data like `_ctype_b` (immutable, no shared-state need — unlike stdout/errno). doom renders + is playable.
- **Stage 7** (`8fb201c`) — import library is now a per-symbol **archive** `libc.ndl.a` (mknx --implib writes one .s per export into a dir; Makefile nasm+ar). Linker pulls only referenced members → a program imports only the symbols it uses (free 121→5, cat→13, nsh→17), not all ~130. Data slots (stdout/errno) are slot-only archive members too, so dllimport.S is gone. This closed the "eager + import-all" startup-cost gap.

Key mknx rules learned: imports derived from `__imp_<name>` global symbols. Classify by **section flags** not st_type (picolibc memcpy/strlen are STT_NOTYPE in .text). Skip relocs whose symbol is SHN_UNDEF/SHN_ABS (else a NULL becomes `delta` and faults). Data-symbol rule: **stateful** data (stdout/errno) → dllimport `__imp_`; **const** tables (_ctype_b) → static local copy via `-lc` fallback. See [[nanos-multiprocessing-roadmap]].

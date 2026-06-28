# Native GCC Toolchain for NanOS — Design

**Status:** approved design, pre-plan
**Date:** 2026-06-28
**Author:** brainstormed with the maintainer
**Related:** [[nanos-sdk-and-vim-port]], [[nanos-bash-port]], [[nanos-git-port]],
[[nanos-pthread-port]], [[nanos-dll-dynamic-linking]], [[nanos-ext-readwrite]],
[[nanos-tls-ssl-ssh]], [[no-shortcuts-on-foundations]]

---

## 1. Goal

Make NanOS able to **compile software on itself**. You sit at the NanOS shell and:

```
$ gcc -O2 hello.c -o hello.nxe && ./hello.nxe
$ g++ -O2 demo.cpp -o demo.nxe && ./demo.nxe
$ make            # builds a real multi-file C/C++ project on-device
```

The headline deliverable is a **native, modern, self-hosting GNU toolchain** for NanOS —
`gcc`/`g++`, `cc1`/`cc1plus`, the full binutils, `libgcc`, `libstdc++`, a real on-disk
sysroot, and `mknx` — all running **on NanOS**, producing `.nxe`s that link dynamically
against `libc.ndl` (and `libstdc++.ndl` for C++) exactly like every other app on the system.

This is explicitly a **no-shortcuts foundation** ([[no-shortcuts-on-foundations]]). The two
hard correctness bars below are non-negotiable parts of "done":

1. **Upstream test suites pass** — the real binutils + GCC (C and C++) dejagnu suites, with
   compiled binaries executed **on NanOS** via a remote DejaGnu board, not a smoke test.
2. **3-stage self-hosting bootstrap** — the native gcc rebuilds gcc on NanOS:
   stage1 → stage2 → stage3, with **stage2 bit-identical to stage3** (GCC's
   `compare` check). This is *the* canonical proof a native compiler is correct.

Out of the day-to-day target but acknowledged: cross-compiling from NanOS to other
targets; Fortran/Go/etc. front ends; LTO plugin. Each is a clean future add, not a
shortcut taken here.

---

## 2. Why this is achievable now

NanOS recently acquired every primitive a native toolchain needs:

- **Process model** — `fork` / `execve` / `waitpid`, pipes, job control
  ([[nanos-multiprocessing-roadmap]]). The gcc driver spawns `cc1`/`as`/`ld` as
  subprocesses via libiberty `pex-unix` (fork+exec+wait+tmpfiles).
- **Threads + real `munmap`** ([[nanos-pthread-port]]) — gcc/g++ are not threaded by
  default, but `libstdc++` and a robust malloc want real `mmap`/`munmap`.
- **ext4 read-write + JBD2** ([[nanos-ext-readwrite]]) and **`/tmp` RamFs** — for object
  files, temporaries (`TMPDIR=/tmp`), and the on-disk source tree the bootstrap consumes.
- **A complete C library** ([[nanos-bash-port]]) — `libc.ndl` is export-all picolibc +
  libc-glue; vim/git/bash/grep/htop/sqlite already exercise it hard.
- **Dynamic loader / import-by-name** ([[nanos-dll-dynamic-linking]]) — output `.nxe`s
  import `libc.ndl` (and will import `libstdc++.ndl`); `mknx` writes the needed table.
- **The cross toolchain already exists** ([[nanos-sdk-and-vim-port]]) — `nanos-sdk` ships
  patched `binutils-2.43` + `gcc-14.2` + `picolibc` with full `x86_64-nanos` **target**
  support, emitting `.nxe`s today. We reuse those exact target patches.
- **SSH + TCP/IP** ([[nanos-tls-ssl-ssh]]) — Dropbear runs; this is the transport for the
  remote DejaGnu board that runs the upstream test suites against NanOS.

---

## 3. Core mechanism — the "cross-native" (crossback) build

Building a compiler that *runs on* a new platform is standard, well-trodden procedure —
the opposite of a shortcut. We use the existing SDK cross toolchain as the bootstrap
compiler and reconfigure the **same patched sources** with a different `--host`:

| Build kind | build | host | target | Exists? |
|---|---|---|---|---|
| Cross (today) | linux | linux | x86_64-nanos | ✅ in `nanos-sdk` |
| **Native (this project)** | linux | **x86_64-nanos** | x86_64-nanos | ⬅ build this |

Because `host == target == x86_64-nanos` and only the **C++ host compiler** is the SDK
cross g++, this is a *cross-native* build, not a Canadian cross. The decisive payoff:
**the target machine description and the `.nxe`-emitting spec already exist** in the SDK's
patched gcc/binutils. We change `--host`, not the backend. We are not inventing GCC target
support — that is done and proven by the cross compiler.

---

## 4. The on-device compile pipeline

```
$ gcc -O2 a.c b.c -o prog.nxe          # every stage below runs ON NanOS, spawned via fork/exec/wait
    cc1  a.c        → /tmp/a.s          C → x86-64 asm           (gcc C front end)
    as   /tmp/a.s   → /tmp/a.o          asm → ELF object         (binutils gas)
    cc1  b.c        → /tmp/b.s ; as → /tmp/b.o
    ld   crt0.o a.o b.o -lc_stubs -lgcc → /tmp/prog.elf          (binutils ld + nx.ld)
    mknx --need libc.ndl /tmp/prog.elf → prog.nxe                (ELF → dynamic .nxe)
```

- The final **ELF → `.nxe` wrap is a GCC *spec* post-link step** — the *same* spec the
  cross SDK uses — so native `gcc` behaves byte-for-byte like cross `x86_64-nanos-gcc`.
- C++ adds `cc1plus` for `.cpp`, and `-lstdc++` (resolved to `libstdc++.ndl` import stubs);
  the output `.nxe` declares `--need libc.ndl --need libstdc++.ndl`.
- `mknx` is `tools/mknx.c` — **plain C**, so it cross-compiles to a NanOS `.nxe` with no
  Python on the target. It derives `--need` from each import's `.nxlib.<lib>` tag
  ([[nanos-sdk-and-vim-port]] mknx fix), so the needed table can't lie.
- Scratch dir is `/tmp` (RamFs, writable); the compiler runs from a writable cwd
  (`/disks/main` or `/tmp`) to avoid the SynthFs-root EROFS class of bug seen in the git
  port ([[nanos-git-port]] `git -C`).

---

## 5. Repository & integration layout

**The port lives in the existing `nanos-sdk` repo** (already separate from NanOS — the
"osobne repo" requirement is met). Rationale: the `*-nanos` **target** patches that make
`.nxe` emission real already live there; a native build is the same sources with a
different `--host`. A third repo would fork those patches and create the exact duplicated
foundation [[no-shortcuts-on-foundations]] warns against.

```
nanos-sdk/                      (separate public repo)
  toolchain/                    existing cross toolchain (build=linux,host=linux,target=nanos)
  native/                       NEW: cross-native build recipes (host=nanos)
    build-binutils-native.sh    configure --host=x86_64-nanos … binutils-2.43
    build-gcc-native.sh         configure --host=x86_64-nanos … gcc-14.2 (C + C++)
    build-libstdcxx.sh          → libstdc++.a + libstdc++.ndl import stubs + headers
    sysroot-ondisk/             the /nanos dev sysroot staged into the NanOS image
    config.cache                honest, probed cache (see §7) — NOT guessed
  nanos-sdk-work/native-toolchain/   build artifacts (gitignored, like the other ports)

NanOS/
  Makefile                      NEW target `make gcc` (a.k.a. `make toolchain`) — stages
                                bin/{gcc,g++,cpp,cc1,cc1plus,as,ld,ar,ranlib,objcopy,nm,
                                strip,objdump,addr2line,mknx}.nxe + the /nanos sysroot;
                                `_image64` installs them. NEVER breaks `make image64` when
                                the SDK work dir is absent (mirrors `make vim`/`make htop`).
  docs/superpowers/specs/2026-06-28-native-gcc-toolchain-design.md   (this file)
  docs/superpowers/plans/2026-06-28-native-gcc-toolchain.md          (implementation plan)
```

On-disk runtime layout (per [[nanos-sdk-and-vim-port]] / docs/en/filesystem.md):

```
/nanos/bin/     gcc g++ cpp cc1 cc1plus as ld ar ranlib objcopy nm strip objdump mknx make …
/nanos/lib/     libc.ndl  libstdc++.ndl  + import-stub .a's, crt0.o, crtbegin/end.o,
                libgcc.a, nx.ld, gcc's internal libexec (cc1/cc1plus live here in GCC layout)
/nanos/include/ full C + C++ headers (the SDK's posix-hosted-patched sysroot, verbatim)
```

---

## 6. Decomposition — phases

One umbrella spec; each phase gets sections in the implementation plan and lands value
independently. Ordering puts the load-bearing/risky pieces first and the bootstrap last.

| Phase | Deliverable | Done when |
|---|---|---|
| **0 — OS de-risk probe (optional, throwaway)** | A 1-day TinyCC spike. tcc is one self-contained binary (own asm+linker). **Not** a deliverable and discarded after — a cheap diagnostic that exercises the OS compile-loop (fork/exec churn, `/tmp`, malloc pressure, cwd, tmpfile lifetimes) before the heavy GCC builds. | tcc compiles+runs hello.c on-device, OR we decide the signal isn't worth it and skip. Any OS gaps it surfaces become fixes before Phase 1. |
| **1 — Native binutils** | `as, ld, ar, ranlib, objcopy, nm, strip, objdump, addr2line` built host=x86_64-nanos. | On NanOS: hand-write `.s` → `as` → `ld` (crt0 + stub) → `mknx` → **runs**, and output is bit-identical to the cross-built equivalent. The `ld`/`gas` dejagnu suites pass via the remote board (§8). |
| **2 — `mknx` + C sysroot on disk** | native `mknx`; ship full C headers, import-stub `libc.a`, `crt0.o`, `crtbegin/end.o`, `libgcc.a` placeholder, `nx.ld` to `/nanos`. | A hand-assembled object links against the on-disk sysroot and runs; sysroot headers byte-match the SDK cross sysroot. |
| **3 — Native gcc (C)** | `gcc` driver + `cc1` + real `libgcc.a`, reusing the SDK nanos target/spec. | On NanOS: compile hello.c → multi-file → **rebuild an existing nsh coreutil** (e.g. `true.c`, `echo.c`) and run it. Output bit-identical to cross gcc for the same inputs. `gcc.c-torture` + `gcc.dg` pass via the board. |
| **4 — Native g++ + libstdc++** | `cc1plus`; `libstdc++` as `libstdc++.ndl` (+ static `.a`) + C++ headers. | On NanOS: compile a real C++ program (STL containers, iostreams, exceptions, RTTI) and run it. `g++.dg` + `libstdc++` testsuite pass via the board. |
| **5 — GNU make + dev-shell packaging** | GNU make ported (small SDK port); curate gcc+binutils+make + the already-ported coreutils/grep/sed/git + headers/libs into a coherent `/nanos` dev environment. | `make smoke-cc` gate (§8): boot NanOS → `make` a multi-file C **and** C++ project from a Makefile → run the results. |
| **6 — 3-stage self-hosting bootstrap (the bar)** | The gcc-14.2 source tree on the NanOS disk is built **by the native toolchain on NanOS**: `configure` (on-device) + `make bootstrap`, producing stage1→stage2→stage3. | `make compare` passes: **stage2 == stage3 bit-identical**. This is the canonical correctness proof. RAM/time tuning + OOM debugging are part of this phase. |

If on-device `configure` (thousands of shell/fork invocations) proves to be the dominant
blocker in Phase 6, the **fallback is a pre-generated, audited build tree**: configure is
run by the cross host to produce the `Makefile`/`config.h`, then `make bootstrap` runs
entirely on NanOS with the **native compiler doing 100% of the compilation**. The bit-exact
stage2==stage3 proof is unchanged; only the (deterministic, non-compiling) configure step is
staged. This is documented as a known, bounded compromise with a `# why:` note, not a silent
shortcut — and full on-device configure remains the goal.

---

## 7. No-shortcuts commitments (explicit)

These are requirements, audited per phase:

1. **Full features, real `configure`.** Full binutils; gcc C **and** C++ front ends; real
   `libgcc`, real `libstdc++`. Any disabled feature carries a `# why:` note tied to a
   genuine platform fact (e.g. *no ELF shared libraries — NanOS links `.ndl`, not `.so`*;
   *no LTO plugin in phase 1 — needs `dlopen` of a host plugin, tracked separately*).
   "Disabled because it failed to build" is never acceptable — it gets fixed.
2. **Honest autoconf.** Config values come from real probes via the SDK honest-conftest
   mechanism ([[nanos-sdk-and-vim-port]]: `conftest-strict-wrapper.sh` +
   `gen-conftest-stubs.sh`), and `config.cache` / generated `config.h` are **audited**, not
   guessed and patched reactively.
3. **A real, complete sysroot.** Full headers + real import stubs + crt + `libgcc.a` +
   `libstdc++` + `nx.ld` — not a curated subset. The on-disk headers byte-match the SDK
   cross sysroot so on-device and cross compiles agree exactly.
4. **`mknx` ported as real C**, not a wrapper hack.
5. **Upstream test suites run** (§8), not link-and-smoke checks. This is the
   [[no-shortcuts-on-foundations]] "verify against the upstream testsuite" rule made
   concrete.
6. **The bootstrap is real** — stage2==stage3, with no `--disable-bootstrap` escape.

---

## 8. Testing strategy

Three independent layers; all are required.

**(a) Differential vs the cross compiler (the oracle).** The native and cross compilers are
the *same sources/patches* differing only in `--host`, so for identical inputs+flags their
generated `.s`/`.o`/`.nxe` must match. Per phase, compile a corpus on-device and on the cross
host and **diff the artifacts** (and, where nondeterminism exists, require identical *runtime
behavior*). Fast, runs in CI on the cross side.

**(b) Upstream dejagnu suites, executed on NanOS.** Run binutils `gas`/`ld`, `gcc.c-torture`,
`gcc.dg`, `g++.dg`, and the `libstdc++` testsuite from the Linux host's dejagnu, with a
**custom DejaGnu board** (`nanos-ssh.exp`) that copies each compiled binary to a running
NanOS guest over SSH (Dropbear, [[nanos-tls-ssl-ssh]]) / scp-equivalent, runs it, and returns
the exit status + stdout. This validates **code generation rigorously** against the real
compiler test corpus. Failures are triaged into: NanOS libc/kernel gaps (fix), genuine
platform N/A (annotate the expected-fail list with a `# why:`), or compiler bugs (fix the
port). The board config and an expected-results baseline are committed.

**(c) Self-hosting bootstrap.** `make bootstrap` + `make compare` on NanOS, stage2==stage3
(Phase 6). The ultimate end-to-end test: the compiler is correct enough to reproduce itself
bit-for-bit.

**CI gate in NanOS:** `make smoke-cc` — a headless-QEMU gate (the established
`-display none` + monitor `screendump` pattern) that boots NanOS, compiles a multi-file C and
a C++ program via `make`, runs them, and checks output. Added to `verify64`.

---

## 9. Risks & mitigations

| Risk | Mitigation |
|---|---|
| **RAM** — `cc1plus`/bootstrap peak at hundreds of MB–GBs. | NanOS memory map is RAM-agnostic to 512 MiB+ ([[nanos-x64-paging-carryforward]], [[nanos-meminfo-memtotal]]); give QEMU 4–8 GiB. Headline compiles target single files/coreutils; bootstrap gets a dedicated RAM-tuning task. Real `mmap`/`munmap` already exist; watch picolibc malloc fragmentation, add a better allocator if bootstrap thrashes. |
| **Time** — bootstrap is hours on-device. | Acceptable for a one-shot correctness proof; not on the interactive path. The smoke gate compiles small programs, not gcc. |
| **C++ exceptions / RTTI / unwind** on NanOS. | `libstdc++` + `libgcc` `_Unwind_*` must work in `.nxe`s. Exercised directly in Phase 4 by the `g++.dg`/`libstdc++` suites before bootstrap depends on it. |
| **On-device `configure` cost** (Phase 6). | Pre-generated audited build tree fallback (§6) keeps the compile work 100% native while staging only the deterministic configure step, with a `# why:` note. |
| **cwd/EROFS on SynthFs root.** | `TMPDIR=/tmp`; run from a writable cwd; verified in Phase 0/1. |
| **Disk space** for the gcc source tree + objects. | ext4 rw image sized up for the bootstrap; objects in `/tmp` where possible. |

---

## 10. Open items deferred (not shortcuts — scoped future work)

- Fortran/Go/Ada front ends; LTO plugin; gcc as a *cross* compiler hosted on NanOS.
- `gdb` on-device (separate port; complements the toolchain).
- Replacing the picolibc allocator if bootstrap memory behavior demands it.

Each is an additive future project with its own spec, explicitly out of this deliverable.

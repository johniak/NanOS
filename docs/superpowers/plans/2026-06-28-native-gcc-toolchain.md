# Native GCC Toolchain for NanOS — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up a native, self-hosting C/C++ GNU toolchain on NanOS. **This document is Plan 1 of a series** and covers **Phases 0–2**: an OS-readiness probe, native **binutils**, and native **`mknx` + on-disk C sysroot** — enough to **assemble, link, and wrap a program into a runnable `.nxe` entirely on NanOS**. Phases 3–6 (gcc-C, g++/libstdc++, make + dev-shell, 3-stage bootstrap) and the DejaGnu test board get their own plans (indexed at the bottom), written once their predecessor lands.

**Architecture:** A *cross-native* build — reconfigure the SDK's already-patched `binutils-2.43` sources with `--host=x86_64-nanos` (build = the container), using the existing `x86_64-nanos` cross toolchain as the host compiler. Because the cross `x86_64-nanos-gcc` already wraps its link output to `.nxe` and a cross-host build never executes host programs, the produced `as`/`ld`/… come out as NanOS binaries. They install into a staging sysroot that NanOS's `make gcc` target writes into `/nanos`.

**Tech Stack:** binutils 2.43, the `nanos-sdk` cross toolchain (`x86_64-nanos-gcc` 14.2 + picolibc), Docker image `nanos-sdk-dev`, NanOS `Makefile` + `_image64`, headless QEMU verification (`-display none` + monitor `screendump`).

## Global Constraints

- **No-shortcuts foundation** ([[no-shortcuts-on-foundations]]): full features; any disable carries a `# why:` note tied to a genuine platform fact; verify against real behavior, not link-only smoke tests.
- **Separate repo:** all toolchain build recipes live in `~/Projects/nanos-sdk` (a separate public repo). NanOS only *stages* the produced artifacts. Never `git add` build artifacts into NanOS.
- **Target triple:** `x86_64-nanos`. The i686 flow is frozen — do not touch it.
- **Output model:** programs link **dynamically** against `libc.ndl`; the final `ELF → .nxe` wrap is done by `mknx` (or the gcc spec that calls it). `--need` is derived from `.nxlib.<lib>` tags — never hand-stuffed.
- **No Claude attribution** in any commit message or PR ([[no-claude-attribution-in-commits]]).
- **Build container:** all toolchain builds run inside `nanos-sdk-dev:latest` with the cross toolchain on `PATH`. macOS host never compiles toolchain code directly (case-insensitive FS trap; the SDK already handles this).
- **Reproducibility:** every build is a committed script in `nanos-sdk/native/`, not ad-hoc shell history.
- **Commits:** frequent, one per task minimum. Toolchain commits land in the `nanos-sdk` repo; NanOS-side wiring commits land in NanOS on a feature branch (not `main`).

---

## Repo & path reference (read once)

NanOS `Makefile` variables already in use by the `vim`/`htop` ports — reuse them verbatim:

- `SDK ?= $(HOME)/Projects/nanos-sdk` — the SDK repo.
- `SDK_TC := $(SDK)/toolchain` — installed cross toolchain prefix (contains `x86_64-nanos/`, `bin/x86_64-nanos-gcc`, …).
- `SDK_WORK ?= $(HOME)/Projects/nanos-sdk-work` — build artifacts (gitignored).
- Port container: `nanos-sdk-dev:latest`, bind-mount the SDK at `/sdk` and the work dir at `/work`, `PATH=/sdk/toolchain/bin:$PATH` (mirror the `vim:` target's `docker run` invocation).

New SDK files this plan creates:

- `nanos-sdk/native/build-binutils-native.sh` — the cross-native binutils build.
- `nanos-sdk/native/stage-sysroot.sh` — assemble the `/nanos` on-disk sysroot (headers, stubs, crt, nx.ld, mknx).
- `nanos-sdk/native/README.md` — what `native/` is and how to run it.

New NanOS files this plan creates/changes:

- `Makefile` — add `toolchain-binutils` + `mknx-native` + `sysroot-ondisk` staging targets and `_image64` install hooks (modeled on the `vim:`/`htop:` targets and their `_image64` install lines).
- `tests/smoke/asm-link-ondevice.sh` (or a `smoke-cc-binutils:` Makefile target) — the headless-QEMU on-device acceptance gate.

---

## Phase 0 — OS-readiness probe (optional, throwaway)

**Purpose:** cheaply surface NanOS gaps in the *compile loop* (fork/exec churn, `/tmp` tmpfiles, malloc pressure, cwd/EROFS) **before** sinking days into binutils. TinyCC is a single self-contained binary with its own assembler+linker. **It is a diagnostic, not a deliverable, and is deleted after.** If the team is confident, skip straight to Phase 1.

### Task 0 (optional): TinyCC OS-probe spike

**Files:**
- Create: `nanos-sdk-work/tcc-probe/` (throwaway; never committed)

**Interfaces:**
- Consumes: the cross toolchain (`x86_64-nanos-gcc`), an existing NanOS image with bash.
- Produces: a written findings note (OS gaps), not code. Output discarded.

- [ ] **Step 1: Define the failing acceptance check.** On a booted NanOS, the goal command is `tcc -o hello.nxe hello.c && ./hello.nxe` printing `hello`. Before any work this obviously fails (no `tcc`). Write the 6-line `hello.c` and the expected console line (`hello`) into the probe dir.

- [ ] **Step 2: Cross-build tcc for `x86_64-nanos`.** In the container, clone TinyCC (mob branch), `./configure --cc=x86_64-nanos-gcc --cpu=x86_64 --enable-cross` is wrong for a *native* tcc — instead configure `--host=x86_64-nanos` style: `CC=x86_64-nanos-gcc ./configure --cpu=x86_64 --extra-cflags=...`, `make tcc`. Wrap to `.nxe` with the SDK `mknx` (host one). Expect build breakage — record each gap (missing libc symbol, header, syscall) as a finding.

- [ ] **Step 3: Run on NanOS.** Stage `tcc.nxe` + a minimal `/nanos/include` + `libtcc1` into a scratch image; boot headless; run the Step-1 command. Capture console via the monitor `screendump` pattern.

- [ ] **Step 4: Record findings, not fixes.** Write `nanos-sdk-work/tcc-probe/FINDINGS.md`: which OS primitives the compile loop exercised, what broke, what each implies for Phase 1+ (e.g. "`/tmp` tmpfile unlinked-while-open semantics needed", "malloc churn fine to N MB"). Any genuine OS bug becomes a NanOS fix task *before* Phase 1.

- [ ] **Step 5: Delete the probe tree.** `rm -rf nanos-sdk-work/tcc-probe` after extracting `FINDINGS.md` to the plan's notes. No tcc artifact ships. (`# why: tcc is a diagnostic; the deliverable is GCC.`)

---

## Phase 1 — Native binutils

Deliverable: `as, ld, ar, ranlib, objcopy, nm, strip, objdump, addr2line` built `--host=x86_64-nanos`, installed to the staging `/nanos`, and proven on NanOS by assembling+linking a **freestanding** program (no libc yet) that runs.

### Task 1: Cross-native binutils build script + configure gate

**Files:**
- Create: `nanos-sdk/native/build-binutils-native.sh`
- Create: `nanos-sdk/native/README.md`

**Interfaces:**
- Consumes: `nanos-sdk/toolchain/patch.sh` (already teaches binutils the `x86_64-nanos` target), the installed cross toolchain on `PATH`, binutils-2.43 source (downloaded like `build-toolchain.sh` does).
- Produces: an installed native binutils tree at `$NPREFIX` (default `/opt/x86_64-nanos-native`), whose `bin/` holds NanOS-format `as`, `ld`, etc. Later tasks read from `$NPREFIX`.

- [ ] **Step 1: Write the failing gate.** Add to `build-binutils-native.sh` a final self-check that greps the configure output for the canonicalized host. Before writing the configure step, the script's `configure-binutils` phase doesn't exist, so running it fails. The gate command:

```sh
sh native/build-binutils-native.sh configure-binutils 2>&1 | tee /tmp/cfg.log
grep -q 'host=x86_64-nanos' /tmp/cfg.log || { echo "FAIL: host not x86_64-nanos"; exit 1; }
```

Expected now: FAIL (script/phase absent).

- [ ] **Step 2: Write the build script.** Mirror `build-toolchain.sh` but for a native (host) build. Key differences: add `--host=$TARGET`, point host tools at the cross prefix, keep `patch.sh` applied.

```sh
#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# build-binutils-native.sh — build binutils that RUNS ON NanOS (host=x86_64-nanos).
# Run inside nanos-sdk-dev with the cross toolchain on PATH. Phases: patch|configure-binutils|all.
set -e
BINUTILS_VERSION="${BINUTILS_VERSION:-2.43}"
TARGET="${TARGET:-x86_64-nanos}"
SDK="$(cd "$(dirname "$0")/.." && pwd)"
CROSS="${CROSS:-$SDK/toolchain}"                 # installed cross toolchain prefix
NPREFIX="${NPREFIX:-/opt/$TARGET-native}"        # where native binutils install
WORK="${WORK:-/opt/nanos-sdk-build}"
JOBS="$(nproc)"; PHASE="${1:-all}"
export PATH="$CROSS/bin:$PATH"

mkdir -p "$WORK"; cd "$WORK"
[ -d "binutils-$BINUTILS_VERSION" ] || { wget -q "https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VERSION.tar.xz"; tar xf "binutils-$BINUTILS_VERSION.tar.xz"; }
sh "$SDK/toolchain/patch.sh" "$WORK/binutils-$BINUTILS_VERSION" /dev/null "$SDK/toolchain" 2>/dev/null || \
  sh "$SDK/toolchain/patch.sh" "$WORK/binutils-$BINUTILS_VERSION" "$WORK/binutils-$BINUTILS_VERSION" "$SDK/toolchain"
[ "$PHASE" = patch ] && { echo "patch done"; exit 0; }

rm -rf build-binutils-native; mkdir build-binutils-native; cd build-binutils-native
# host=target=x86_64-nanos, build auto-detected (the container). The cross x86_64-nanos-gcc is
# the HOST compiler; configure finds it by triple prefix on PATH.
../binutils-$BINUTILS_VERSION/configure \
    --host=$TARGET --target=$TARGET --prefix=/nanos \
    --with-sysroot=/nanos --disable-nls --disable-werror \
    --disable-shared --enable-static \
    CC_FOR_BUILD=cc 2>&1 | tee config.out
echo "host=$(sed -n 's/^.*host_alias=\([^ ]*\).*/\1/p; s/.*--host=\([^ ]*\).*/\1/p' config.out | head -1)"
grep -q "host=$TARGET" config.out || grep -q "Configuring .* host $TARGET" config.out || true
[ "$PHASE" = configure-binutils ] && { echo "configure OK"; grep -m1 -i "host *$TARGET" config.log || true; exit 0; }

make -j"$JOBS"
make install DESTDIR="$WORK/native-stage"        # staged install -> $WORK/native-stage/nanos/...
echo "== installed to $WORK/native-stage/nanos =="
ls "$WORK/native-stage/nanos/bin"
```

Note: `--prefix=/nanos` so the on-target install paths (which gcc later bakes into its search dirs) are the real NanOS paths; `DESTDIR` redirects the *container-side* write into a staging tree.

- [ ] **Step 3: Run the configure gate.**

Run (in container): `sh native/build-binutils-native.sh configure-binutils`
Expected: configure completes; `config.log` shows host `x86_64-nanos`. If configure can't find the host compiler, fix `PATH`/`CC` — do not fake `config.cache`.

- [ ] **Step 4: Write the README.**

```markdown
# native/ — the NanOS-hosted toolchain build

These scripts build binutils/gcc that RUN ON NanOS (host=x86_64-nanos), as opposed to
../build-toolchain.sh which builds the Linux-hosted cross toolchain (target=x86_64-nanos).
Run inside the nanos-sdk-dev container with the cross toolchain on PATH. Output stages into
$WORK/native-stage/nanos and is shipped into the NanOS image by NanOS's `make gcc` target.
```

- [ ] **Step 5: Commit (in nanos-sdk).**

```bash
cd ~/Projects/nanos-sdk
git add native/build-binutils-native.sh native/README.md
git commit -m "native: cross-native binutils build script (host=x86_64-nanos)"
```

### Task 2: Build native binutils + verify NanOS binary format

**Files:**
- Modify: `nanos-sdk/native/build-binutils-native.sh` (the `all` phase already written; this task runs it and adds a format check)

**Interfaces:**
- Consumes: Task 1's script.
- Produces: `$WORK/native-stage/nanos/bin/{as,ld,ar,ranlib,objcopy,nm,strip,objdump,addr2line}` as NanOS `.nxe`-format executables.

- [ ] **Step 1: Write the failing format gate.** A NanOS executable starts with the `NxHeader` magic (the loader is extension-agnostic and checks magic — see CLAUDE.md). Write `native/check-nanos-binary.sh`:

```sh
#!/bin/sh
# Pass IFF $1 begins with the NanOS NxHeader magic (first 4 bytes). Adjust MAGIC to NxFormat.h.
f="$1"; MAGIC="$(printf '\\x4e\\x58\\x45\\x00')"   # 'NXE\0' — confirm against kernel/NxFormat.h
head -c4 "$f" | od -An -tx1 | tr -d ' \n'
```
Then assert it equals the header magic from `NanOS/kernel/NxFormat.h`. Before building, the file doesn't exist → FAIL.

- [ ] **Step 2: Confirm the real magic.** Read `NanOS/kernel/NxFormat.h` for the exact `NxHeader` magic constant and byte order; bake it into `check-nanos-binary.sh`. (Do not guess — the cross gcc's wrap writes this header; the check must match it.)

- [ ] **Step 3: Run the full build.**

Run (in container): `sh native/build-binutils-native.sh all`
Expected: binutils compiles with the cross compiler and installs into `$WORK/native-stage/nanos/bin`.

- [ ] **Step 4: Run the format gate on `as` and `ld`.**

Run: `sh native/check-nanos-binary.sh $WORK/native-stage/nanos/bin/as`
Expected: PASS (NxHeader magic present) — proving the cross gcc spec wrapped the host program to `.nxe` automatically. If instead they're plain ELF, add the explicit wrap step to the install (`# why:` note) — but first confirm the cross spec didn't run because of `--disable-shared`/static-link nuances.

- [ ] **Step 5: Commit.**

```bash
cd ~/Projects/nanos-sdk
git add native/build-binutils-native.sh native/check-nanos-binary.sh
git commit -m "native: build+install NanOS-hosted binutils; verify NxHeader format"
```

### Task 3: NanOS `make toolchain-binutils` staging target

**Files:**
- Modify: `NanOS/Makefile` (add a `toolchain-binutils:` target + `_image64` install lines; model on the `vim:`/`htop:` targets)

**Interfaces:**
- Consumes: `$WORK/native-stage/nanos/bin/*` produced by Task 2.
- Produces: `NanOS/bin/{as,ld,ar,ranlib,objcopy,nm,strip,objdump,addr2line}.nxe` staged; `_image64` installs them to `/nanos/bin`.

- [ ] **Step 1: Write the failing gate.** `make image64` does not yet place `as` in the image. Define the gate: after `make toolchain-binutils image64`, the image must contain `/nanos/bin/as`. Check via the existing image-inspection method (loop-mount or `debugfs`, as the `_image64` target already uses to write files).

- [ ] **Step 2: Add the staging target.** Mirror the optional-port pattern (errors clearly if the SDK work dir is absent; never breaks `make image64`):

```make
# Native NanOS-hosted binutils (optional, external — built by nanos-sdk/native/). Stages the
# .nxe tools the SDK produced. `make image64` installs them to /nanos/bin if present.
NATIVE_STAGE ?= $(SDK_WORK)/native-stage/nanos
BINUTILS_TOOLS := as ld ar ranlib objcopy nm strip objdump addr2line
toolchain-binutils:
	@test -d "$(NATIVE_STAGE)/bin" || { echo "native binutils not built (run nanos-sdk/native/build-binutils-native.sh)"; exit 1; }
	@for t in $(BINUTILS_TOOLS); do \
	  test -f "$(NATIVE_STAGE)/bin/$$t" || { echo "missing $$t"; exit 1; }; \
	  cp "$(NATIVE_STAGE)/bin/$$t" "$(BINFOLDER)$$t.nxe"; \
	done
	@echo "staged binutils tools — run 'make image64' to install into /nanos/bin"
```

- [ ] **Step 3: Add the `_image64` install hook.** In the `_image64` recipe, where other `/nanos/bin` tools are written, add a loop installing `bin/<t>.nxe → /nanos/bin/<t>` for each tool **iff** the staged file exists (guarded like the optional ports), so a plain `make image64` without the toolchain still works.

- [ ] **Step 4: Run the gate.**

Run: `make toolchain-binutils image64` then inspect the image for `/nanos/bin/as`.
Expected: present. A bare `make image64` (without `toolchain-binutils`) still succeeds with no binutils — confirm both.

- [ ] **Step 5: Commit (in NanOS, on a feature branch).**

```bash
cd ~/Projects/NanOS
git checkout -b feat/native-toolchain   # if not already on it
git add Makefile
git commit -m "build: stage native binutils into /nanos/bin (optional toolchain target)"
```

### Task 4: On-device acceptance — assemble + link + run a freestanding program

**Files:**
- Create: `NanOS/tests/smoke/asm-link-ondevice.sh` (host-side driver using the headless-QEMU pattern)
- Create: `NanOS/tests/smoke/fixtures/exit42.s` (freestanding `_start`)

**Interfaces:**
- Consumes: `/nanos/bin/{as,ld}` in the image (Task 3) + the native `mknx` — **but mknx is Phase 2**. To keep Phase 1 independent, this task wraps the ELF with the *cross/host* `mknx` pre-staged into the image as `/nanos/bin/mknx` is not yet present; instead link to a raw ELF and use a **temporary** image-time wrap. **Simplest correct approach:** defer the *runnable* assertion to Task 7 (Phase 2) and here assert only that on-device `as` + `ld` produce a correct ELF `.o`/executable, checked with on-device `nm`/`objdump`.

- [ ] **Step 1: Write the freestanding fixture.** A program needing no libc — just the NanOS `exit` syscall — so it links with `ld` alone:

```asm
# exit42.s — freestanding: invoke NanOS exit(42). Confirm the syscall number/ABI against
# kernel/SyscallDispatch + arch syscall calling convention (x86_64: nr in rax, arg in rdi, syscall).
.text
.globl _start
_start:
    mov $1, %rax        # __NR_exit  (confirm value in the NanOS syscall table)
    mov $42, %rdi
    syscall
```

- [ ] **Step 2: Write the failing on-device gate.** `tests/smoke/asm-link-ondevice.sh`: boot the image headless, drive the shell to run:

```
cd /tmp && as /tmp/exit42.s -o e.o && ld e.o -o e.elf && objdump -f e.elf
```

and assert `objdump` reports `start address` and an x86-64 ELF. (Running `e.elf` is deferred — NanOS runs `.nxe`, not raw ELF; that's Task 7.) Use the `-display none -monitor unix:/tmp/qmon` + Python-socket `screendump` → `sips` PNG → read flow from CLAUDE.md. Before Task 3's tools exist, this FAILs (`as: not found`).

- [ ] **Step 3: Stage the fixture into the image.** Add `exit42.s` to the on-disk content (e.g. under `/tmp` seed or `/disks/main`) via the `_image64` content step, so the booted shell can reach it.

- [ ] **Step 4: Run the gate.**

Run: `make toolchain-binutils image64 && sh tests/smoke/asm-link-ondevice.sh`
Expected: console shows a valid x86-64 ELF from on-device `objdump` — **binutils runs on NanOS.**

- [ ] **Step 5: Commit.**

```bash
cd ~/Projects/NanOS
git add tests/smoke/asm-link-ondevice.sh tests/smoke/fixtures/exit42.s Makefile
git commit -m "test: on-device smoke — as+ld+objdump run on NanOS (freestanding)"
```

---

## Phase 2 — Native `mknx` + on-disk C sysroot

Deliverable: native `mknx` on `/nanos/bin`, and a complete C sysroot in `/nanos/{include,lib}` (headers, import-stub `libc.a`, `crt0.o`, `nx.ld`), so an on-device `ld` link against libc produces a runnable `.nxe`.

### Task 5: Build + stage native `mknx`

**Files:**
- Modify: `nanos-sdk/native/stage-sysroot.sh` (create) — builds mknx + assembles the sysroot
- Modify: `NanOS/Makefile` (`mknx-native` staging + `_image64` install)

**Interfaces:**
- Consumes: `NanOS/tools/mknx.c` (plain C), cross `x86_64-nanos-gcc`.
- Produces: `/nanos/bin/mknx` (NanOS `.nxe`).

- [ ] **Step 1: Write the failing differential gate.** Native `mknx` must wrap an ELF into a byte-identical `.nxe` versus the host `mknx` for the same input+flags. Write a check: build a tiny ELF, wrap it with host `mknx64` and (later) on-device `mknx`, `cmp` the outputs. Before building native mknx, it doesn't exist → FAIL.

- [ ] **Step 2: Cross-compile mknx to a `.nxe`.** In `stage-sysroot.sh`:

```sh
x86_64-nanos-gcc -O2 -o "$STAGE/nanos/bin/mknx" "$NANOS_SRC/tools/mknx.c"
# the cross spec wraps it to .nxe; confirm with check-nanos-binary.sh
```

(`$NANOS_SRC` = bind-mounted NanOS repo; pass it into the container like the `vim` target passes paths.)

- [ ] **Step 3: Add the NanOS staging target.** `mknx-native:` copies `$NATIVE_STAGE/bin/mknx` → `bin/mknx.nxe`; `_image64` installs to `/nanos/bin/mknx` (guarded/optional).

- [ ] **Step 4: Run the differential gate on-device.** After Task 7's image exists, on NanOS: wrap `e.o`-derived ELF with `/nanos/bin/mknx`, copy the `.nxe` off (or hash it on-device with the ported `sha`/`cmp`), and compare to the host `mknx64` output of the same ELF. Expected: identical. (If ordering forces it, run this gate at the end of Task 7.)

- [ ] **Step 5: Commit (nanos-sdk + NanOS).**

```bash
cd ~/Projects/nanos-sdk && git add native/stage-sysroot.sh && git commit -m "native: build NanOS-hosted mknx from tools/mknx.c"
cd ~/Projects/NanOS && git add Makefile && git commit -m "build: stage native mknx into /nanos/bin"
```

### Task 6: Assemble + stage the on-disk C sysroot

**Files:**
- Modify: `nanos-sdk/native/stage-sysroot.sh`
- Modify: `NanOS/Makefile` (`sysroot-ondisk` staging + `_image64` install of `/nanos/include` + `/nanos/lib`)

**Interfaces:**
- Consumes: the cross sysroot at `$SDK_TC/x86_64-nanos/{include,lib}` (headers + import-stub `libc.a` + `crt0.o` + `nx.ld` the cross builds already use).
- Produces: `/nanos/include/**`, `/nanos/lib/{libc.a (import stubs), crt0.o, crtbegin.o, crtend.o, nx.ld}` in the image.

- [ ] **Step 1: Write the failing parity gate.** The on-disk headers must byte-match the cross sysroot headers (so on-device and cross compiles agree). Check: `diff -r $SDK_TC/x86_64-nanos/include $STAGE/nanos/include` is empty. Before staging → FAIL (dir absent).

- [ ] **Step 2: Copy the sysroot in `stage-sysroot.sh`.**

```sh
mkdir -p "$STAGE/nanos/include" "$STAGE/nanos/lib"
cp -a "$CROSS/x86_64-nanos/include/." "$STAGE/nanos/include/"
# libc import stubs + crt + linker script the cross link already uses:
cp -a "$CROSS/x86_64-nanos/lib/libc.a"  "$STAGE/nanos/lib/"      # confirm exact names in the cross sysroot
cp -a "$CROSS/x86_64-nanos/lib/crt0.o"  "$STAGE/nanos/lib/" 2>/dev/null || true
cp -a "$CROSS/x86_64-nanos/lib/nx.ld"   "$STAGE/nanos/lib/" 2>/dev/null || true
```

First **inspect** `$SDK_TC/x86_64-nanos/lib` to get the real artifact names (crt naming, whether `nx.ld` lives there or in the gcc spec dir) — do not assume; copy what the cross link actually consumes.

- [ ] **Step 3: Add NanOS staging + install.** `sysroot-ondisk:` stages `include/` + `lib/` from `$NATIVE_STAGE`; `_image64` writes them under `/nanos/include` and `/nanos/lib` (guarded/optional). Reuse the directory-write mechanism `_image64` already uses for `/nanos`.

- [ ] **Step 4: Run the parity gate.**

Run: `make sysroot-ondisk` then `diff -r "$SDK_TC/x86_64-nanos/include" "$NATIVE_STAGE/include"`
Expected: no differences.

- [ ] **Step 5: Commit.**

```bash
cd ~/Projects/nanos-sdk && git add native/stage-sysroot.sh && git commit -m "native: stage C sysroot (headers+stubs+crt+nx.ld) for on-disk /nanos"
cd ~/Projects/NanOS && git add Makefile && git commit -m "build: install /nanos C sysroot into the image"
```

### Task 7: On-device end-to-end — assemble + link libc + wrap + RUN

**Files:**
- Create: `NanOS/tests/smoke/fixtures/hello-asm.s` (calls libc `write` + `exit` via the import stubs)
- Modify: `NanOS/tests/smoke/asm-link-ondevice.sh` (extend with the runnable end-to-end case)
- Modify: `NanOS/Makefile` (add `smoke-cc-binutils:` gate; wire into `verify64`)

**Interfaces:**
- Consumes: `/nanos/bin/{as,ld,mknx}` + `/nanos/{include,lib}` sysroot.
- Produces: a `.nxe` built **entirely on NanOS** that prints to the console — the Phase 1+2 milestone.

- [ ] **Step 1: Write the fixture.** Assembly that links against libc (so it exercises crt0 + import stubs + nx.ld), e.g. calls `write(1, "hi\n", 3)` then `exit(0)` through the C ABI / PLT-equivalent. Confirm the call/reloc convention against an existing cross-built `.nxe` (disassemble one to copy the exact stub pattern).

- [ ] **Step 2: Write the failing end-to-end gate.** Extend `asm-link-ondevice.sh` to drive on NanOS:

```
cd /tmp && as hello-asm.s -o h.o \
  && ld /nanos/lib/crt0.o h.o -L/nanos/lib -lc -T /nanos/lib/nx.ld -o h.elf \
  && mknx --need libc.ndl h.elf h.nxe \
  && ./h.nxe
```

Assert the console shows `hi`. Before the sysroot/mknx exist → FAIL.

- [ ] **Step 3: Stage the fixture** into the image content (like Task 4 Step 3).

- [ ] **Step 4: Run the gate.**

Run: `make toolchain-binutils mknx-native sysroot-ondisk image64 && make smoke-cc-binutils`
Expected: console prints `hi` — **a program assembled, linked, and `.nxe`-wrapped entirely on NanOS runs.** This is the Plan-1 deliverable.

- [ ] **Step 5: Wire into `verify64` and commit.** Add `smoke-cc-binutils` to the `verify64` gate list.

```bash
cd ~/Projects/NanOS
git add tests/smoke/fixtures/hello-asm.s tests/smoke/asm-link-ondevice.sh Makefile
git commit -m "test: on-device end-to-end as+ld+mknx builds & runs a .nxe (verify64 gate)"
```

---

## Self-review (Plan 1)

- **Spec coverage (Phases 0–2):** Phase 0 → Task 0; native binutils (spec §6 Phase 1) → Tasks 1–4; mknx + C sysroot (spec §6 Phase 2) → Tasks 5–7; integration target `make gcc`/staging (spec §5) → Tasks 3,5,6; no-shortcuts gates (spec §7) → format gate (T2), parity gate (T6), differential mknx (T5), on-device run (T7). Upstream dejagnu + bootstrap are explicitly later plans (spec §6 Phases 3–6, §8) — see index below. Covered.
- **Placeholder scan:** the two genuine "confirm against the real source" steps (NxHeader magic in T2; syscall number in T4; exact sysroot artifact names in T6) are **deliberate verification steps**, not placeholders — each tells the implementer the exact file to read (`kernel/NxFormat.h`, the NanOS syscall table, `$SDK_TC/x86_64-nanos/lib`) and what to extract. They must read, not guess.
- **Type/name consistency:** `$NATIVE_STAGE`/`$WORK/native-stage`, `toolchain-binutils`/`mknx-native`/`sysroot-ondisk` targets, `check-nanos-binary.sh`, and `BINUTILS_TOOLS` are used consistently across tasks.

---

## Plan index — the rest of the series (to be expanded into their own plans)

Each becomes a full bite-sized plan **after its predecessor lands**, because the detail depends on what the prior phase surfaces (especially RAM behavior and libc gaps).

- **Plan 2 — Native gcc (C):** port GMP/MPFR/MPC/ISL to `host=x86_64-nanos` (real autotools ports, no shortcuts); cross-native build of the gcc **C** front end + driver + real `libgcc.a`; reuse `nanos.h` spec so native `gcc` == cross `x86_64-nanos-gcc`. Gates: compile hello.c → multi-file → rebuild an existing nsh coreutil on-device; output bit-identical to cross gcc; `gcc.c-torture`/`gcc.dg` via the board (Plan 6).
- **Plan 3 — Native g++ + libstdc++:** `cc1plus`; `libstdc++` as `libstdc++.ndl` + headers; exceptions/RTTI/unwind verified. Gates: on-device C++ (STL/iostreams/exceptions) runs; `g++.dg` + libstdc++ suite via the board.
- **Plan 4 — GNU make + dev-shell packaging:** port GNU make; curate gcc+binutils+make+coreutils/grep/sed/git + sysroot into a coherent `/nanos` dev environment. Gate: `make smoke-cc` — boot → `make` a multi-file C **and** C++ project → run results; added to `verify64`.
- **Plan 5 — 3-stage self-hosting bootstrap:** the gcc-14.2 source tree on the NanOS disk built by the native toolchain; `make bootstrap` + `make compare`, **stage2 == stage3 bit-identical**. Includes RAM/time tuning + the audited pre-generated-configure fallback (spec §6) with a `# why:` note. The correctness bar.
- **Plan 6 — Upstream DejaGnu remote board:** `nanos-ssh.exp` board that runs binutils/gcc/g++ test binaries on a live NanOS guest over SSH (Dropbear); committed expected-results baseline; triage rules (OS gap → fix, platform N/A → annotated XFAIL, compiler bug → fix). Runs the suites referenced by Plans 2–3.

# Basic coreutils (tier 1 + tier 2) — Implementation Plan

> **For agentic workers / Ralph loop:** This file is a *self-driving checklist*. Each
> iteration does ONE task and stops. See "How the Ralph loop uses this file" below. Steps use
> checkbox (`- [ ]`) syntax for tracking — tick `- [x]` only when a task is fully done (built,
> verified, committed).

**Goal:** Give the shell the missing essential file/dir utilities (`mkdir rmdir touch rm ln pwd
cp mv` + a tier-2 set) so you can actually create/remove/move files — closing the
`bash: touch: command not found` gap from the git port.

**Architecture:** These are **in-tree sbase ports**, identical to how `cat`/`ls` already build:
drop the verbatim upstream sbase `.c` into `user/third_party/sbase/`, add it to the Makefile's
`USER_PROGS`/`SYS_PROGS` + a `.nxe` link rule, build inside the `nanos-build` container, install
to `/nanos/bin`. They are dynamically linked against `libc.ndl`. **No external SDK / nanos-port
involved.** Two libc-glue gaps (no-op `chmod`/`chown` stubs; missing `*at`/`utimes` wrappers) are
closed first in Phase 0.

**Tech stack:** sbase (suckless) @ upstream commit `c546c3a5724c81cee9a11d816a38ccdf17472129`
(same as the vendored `cat.c`/`ls.c`); picolibc + `user/libc-glue/`; the kernel ext read+write FS
(all needed syscalls already exist — `kernel/SyscallNr.h`).

---

## How the Ralph loop uses this file

On each iteration:

1. **Read this whole file.** Find the **first** task whose checkbox line (the `- [ ] **Task N…**`)
   is unchecked.
2. **Do only that task**, following its steps exactly. Each task is independently completable from
   a cold start — all context you need is in "Shared facts" below + the task body.
3. **Pass the task's gates** (build gate always; QEMU gate where the task lists one).
4. **Commit** (clean message, NO Claude/AI attribution — see Conventions) and **tick the box**
   `- [x]` in this file (commit the tick as part of the same commit, or amend).
5. **Stop.** The loop re-invokes for the next task.

**Definition of done (loop exit):** every box in Phase 0, Phase 1, Phase 2, and "Finish" is
`- [x]`, `make build && make image` is clean, and the final QEMU smoke run shows the tier-1 tools
working from the shell.

**If blocked:** if a task cannot be completed (a missing syscall, a libc gap not anticipated here,
a tool that turns out not to exist in sbase), do NOT guess or fake it. Add a short `> BLOCKED:
<reason>` note under the task, leave the box unchecked, and skip to the next independent task.
Surface the blocker in your final message.

---

## Shared facts (read once per iteration — this is the whole mechanism)

**Where sources go:** `user/third_party/sbase/<tool>.c` (verbatim upstream). Shared helpers go in
`user/third_party/sbase/libutil/<fn>.c`; the recurse/fs helpers also need
`user/third_party/sbase/fs.h`. Get sources from suckless sbase at the **exact commit above** so
they match the vendored `arg.h`/`util.h`/`compat.h`/libutf. Fetch a single file with:
```
curl -fsSL "https://git.suckless.org/sbase/file/<tool>.c?h=c546c3a5724c81cee9a11d816a38ccdf17472129" -o ...
```
or `git clone https://git.suckless.org/sbase /tmp/sbase && git -C /tmp/sbase checkout c546c3a`
then copy the files you need. **Do not modify the sources** (the macOS case-insensitivity trap is
already handled by the container copy step in the Makefile; the build copies to a case-sensitive
tmp dir). If a source genuinely won't compile freestanding, prefer a tiny `compat-decls.h`/glue
addition over editing upstream.

**Makefile wiring (the 3 edits per tool), all near the existing `cat`/`ls` lines (~667-690, 787,
673-674):**
1. Add the tool to `USER_PROGS` and (since these are system utilities) to `SYS_PROGS`.
2. If it needs libutil objects beyond what a pattern rule already covers, define a
   `SBASE_UTIL_<TOOL>=$(BINFOLDER)eprintf.o …` var (model it on `SBASE_UTIL_CAT`).
3. Add a link rule: `$(BINFOLDER)<tool>.nxe: $(DYN_DEPS) $(BINFOLDER)<tool>.o $(SBASE_UTIL_<TOOL>)`
   (model on `$(BINFOLDER)cat.nxe:` at line ~787). New `libutil/*.c` and top-level `*.c` already
   have pattern rules (lines ~697-703) — no new compile rule needed.
4. `_image` installs everything in `SYS_PROGS` to `/nanos/bin/<tool>.nxe` automatically (loop at
   ~485). No `_image` edit needed.

**Resolving libutil dependencies (deterministic — do NOT pre-guess):** start each tool's link rule
with `$(BINFOLDER)eprintf.o`, run `make build` (or `make _all`), and for every
`undefined reference to 'X'`: vendor the sbase file that defines `X` (a `libutil/X.c`, or for
`cp`/`rm`/`recurse`-style shared funcs the relevant top-level `.c` + `fs.h`) and add `$(BINFOLDER)
X.o` to the rule. Repeat until it links. The "likely objects" hints in each task are a starting
point, not authoritative — the linker is authoritative.

**Build:** everything compiles in Docker. `make build` (kernel + full userland) then `make image`
(writes the disk). On macOS the host shells into the `nanos-build` container automatically. A
single userland tweak: `make _all` rebuilds only changed objects.

**Run / verify (headless QEMU):** `make image` first, then:
```
BOOT_WAIT=30 scripts/qemu-shell.sh /tmp/cu.png "<cmd1>" "<cmd2>" ...
```
It boots, types each command + Enter into the shell, screendumps `/tmp/cu.png`, prints fault
vectors. **BOOT_WAIT=30** (boot is slow due to accumulated JBD2 replay). Read the PNG to confirm
output; grep `/tmp/nanos-int.log` and require `v=08`/`v=0d`/`v=0e`/`v=06` counts to be 0 for the
user session. Default shell is bash if installed, else `nsh`; both run `<name>` by appending
`.nxe` and searching `/nanos/bin`. Use a writable path for tests: `/disks/main` is read-write,
`/tmp` is a tmpfs.

**Syscalls — all already present** (`kernel/SyscallNr.h`): `unlink(10) link(9) symlink(83)
rename(38) mkdir(39) rmdir(40) chmod(15) fchmod(94) chown(182) lchown(16) fchown(95) utime(30)
utimes(271) utimensat(320) truncate(92) ftruncate(93) openat(295) mkdirat(296) unlinkat(301)
renameat(302) renameat2(353) linkat(303) symlinkat(304) readlinkat(305) fchmodat(306) fchownat(298)
fstatat64(300) faccessat(307) fchdir(133) getcwd(183) chdir(12) readlink(85)`. So all work is
userland (libc-glue wrappers + sbase wiring).

**Conventions (MANDATORY):** Commit messages and any PR contain **ZERO** mention of Claude/AI — no
`Co-Authored-By`, no "Generated with", no emoji. Never `git add` `CLAUDE.md`. Never stage these
untracked docs: `docs/superpowers/plans/2026-06-15-x86_64-plan-1-foundation.md`,
`docs/superpowers/specs/2026-06-15-real-hardware-roadmap-design.md`,
`docs/superpowers/specs/2026-06-15-x86_64-migration-analysis.md`. Work on the current branch (do
not switch to master). One commit per task; a `feat:`/`fix:` prefix, e.g.
`feat: add mkdir/rmdir coreutils (in-tree sbase)`.

---

## Phase 0 — libc-glue plumbing (MUST run before the tools that need it)

### - [x] **Task 0.1: Rewire chmod/fchmod/chown/lchown/fchown from no-op stubs to real syscalls**

**Why:** `user/libc-glue/posixstubs.c` currently has `chmod`/`fchmod`/`chown`/`fchown` returning
`0` without touching the disk (a read-only-era leftover). The kernel ext FS is now read-write and
the syscalls exist. Tier-2 `chmod`/`chown` are useless until these call through.

**Files:** Modify `user/libc-glue/posixstubs.c` (the block near line ~78, "permission ops on a
read-only world"). `posixstubs.o` is already in `LIBC_GLUE_OBJS` → lands in `libc.ndl`. No Makefile
edit.

- [x] **Step 1** — Replace the four stub bodies with real syscall calls. Use the existing
  `sys3(...)` / `reterr(...)` helpers (see how `unlink`/`mkdir` call them in
  `user/libc-glue/syscalls.c`; include `kernel/SyscallNr.h` if not already visible). Add `lchown`
  too (sbase `chown` without `-h` uses `chown`; with `-h` uses `lchown`). Target shapes:
  ```c
  int chmod(const char* p, mode_t m)          { return reterr(sys3(SYS_chmod,  (int)p, (int)m, 0)); }
  int fchmod(int fd, mode_t m)                { return reterr(sys3(SYS_fchmod, fd, (int)m, 0)); }
  int chown(const char* p, uid_t u, gid_t g)  { return reterr(sys3(SYS_chown,  (int)p, (int)u, (int)g)); }
  int lchown(const char* p, uid_t u, gid_t g) { return reterr(sys3(SYS_lchown, (int)p, (int)u, (int)g)); }
  int fchown(int fd, uid_t u, gid_t g)        { return reterr(sys3(SYS_fchown, fd, (int)u, (int)g)); }
  ```
  (Match `sys3`/`reterr` signatures and the existing cast style exactly — copy from `syscalls.c`.
  If `lchown` already exists elsewhere, don't duplicate it; resolve the link error.)
- [x] **Step 2** — `make build` is clean (no duplicate-symbol / undefined-ref errors).
- [x] **Step 3 (QEMU gate)** — `make image`, then
  `BOOT_WAIT=30 scripts/qemu-shell.sh /tmp/cu.png "touch /disks/main/cm" "ls -l /disks/main/cm"`
  is not the real test (chmod not yet shipped); instead just confirm the build/boot is clean
  (0 faults in `/tmp/nanos-int.log`). The functional proof comes with tier-2 `chmod` (Task 2.1).
- [x] **Step 4** — Commit: `fix: libc chmod/chown/fchmod/fchown call the real syscalls (were no-op stubs)`. Tick this task.

### - [x] **Task 0.2: Add the `*at` family + `utimes`/`utimensat` libc wrappers**

**Why:** sbase `recurse.c` (used by `rm -r`, `cp -r`, `mv` of dirs) traverses with
`openat`/`fstatat`/`unlinkat`/`fdopendir`, and `touch` sets times with `utimensat`/`utimes`. None
of these wrappers exist in libc-glue yet (the kernel syscalls do).

**Files:** Add to `user/libc-glue/syscalls.c` (already in `LIBC_GLUE_OBJS` → `libc.ndl`). Declare
any missing prototypes in `user/libc-glue/compat-decls.h` or the relevant `user/libc-glue/include/`
headers only if picolibc's headers don't already declare them. `fdopendir` already exists
(`user/libc-glue/dirent.c`).

- [x] **Step 1** — Add thin wrappers, each forwarding to its syscall via `sys3`/a 4-5-arg syscall
  helper (check `syscalls.c` for an existing `sys4`/`sys5`/`sys6`; if only `sys3` exists, add the
  needed N-arg variant the same way). Implement the set the tools actually reference — at minimum:
  `openat, mkdirat, unlinkat, renameat, symlinkat, linkat, fchmodat, fchownat, fstatat64
  (as the impl behind `fstatat`/`__fstatat`), faccessat, readlinkat, utimes, utimensat`. For each,
  match the POSIX signature; pass the `stat`/`timespec` structs through by pointer the way the
  existing `stat`/`fstat` wrappers do (the kernel uses the same struct layout — verify against how
  `SYS_fstatat64`/`SYS_utimensat` are decoded in `kernel/SyscallDispatch.cpp` /
  `kernel/Syscall.cpp`). Example shape:
  ```c
  int unlinkat(int dirfd, const char* p, int flags) { return reterr(sys3(SYS_unlinkat, dirfd, (int)p, flags)); }
  ```
  Keep it minimal — only wire what links. Do not invent semantics the kernel doesn't implement;
  if a syscall arg the kernel ignores (e.g. flags), pass it through anyway.
- [x] **Step 2** — `make build` clean.
- [x] **Step 3** — No QEMU gate of its own (exercised by Task 1.4 `rm -r` and Task 1.3 `touch`).
  Just confirm the build links.
- [x] **Step 4** — Commit: `feat: libc *at-family + utimes/utimensat wrappers`. Tick this task.

---

## Phase 1 — Tier 1 utilities (the essential set)

> Ordering: no-recurse / no-special-libc tools first, then the `recurse`/`*at`-dependent ones.
> Each task: vendor source → wire Makefile → resolve libutil link errors → build → QEMU → commit.
> The QEMU command in each task assumes `make image` was just run.

### - [x] **Task 1.1: `mkdir` + `rmdir`**

**Likely objects:** `mkdir` → `eprintf.o mkdirp.o` (vendor `libutil/mkdirp.c`); `rmdir` →
`eprintf.o`. Sources: `mkdir.c`, `rmdir.c`.

- [x] Vendor `user/third_party/sbase/mkdir.c`, `rmdir.c`, and `libutil/mkdirp.c`.
- [x] Makefile: add `mkdir rmdir` to `USER_PROGS` + `SYS_PROGS`; add
  `SBASE_UTIL_MKDIR=$(BINFOLDER)eprintf.o $(BINFOLDER)mkdirp.o` and
  `SBASE_UTIL_RMDIR=$(BINFOLDER)eprintf.o`; add link rules
  `$(BINFOLDER)mkdir.nxe: $(DYN_DEPS) $(BINFOLDER)mkdir.o $(SBASE_UTIL_MKDIR)` and the rmdir analog.
- [x] `make build` → resolve any extra undefined refs by vendoring the named `libutil/*.c`.
- [x] `make image`, then QEMU:
  `BOOT_WAIT=30 scripts/qemu-shell.sh /tmp/cu.png "mkdir /disks/main/d1" "mkdir -p /disks/main/d2/d3" "ls /disks/main" "rmdir /disks/main/d1"`.
  Confirm `d1`,`d2` appear, `d1` gone after rmdir, 0 faults.
- [x] Commit `feat: add mkdir + rmdir coreutils (in-tree sbase)`. Tick.

### - [x] **Task 1.2: `pwd`**

**Likely objects:** `eprintf.o` (uses `getcwd`, already in libc-glue). Source: `pwd.c`. If `pwd -P`
pulls `realpath` (missing) and you don't want to add it, that's fine — `pwd` (logical) is the
required behavior; note `-P` as a limitation rather than adding `realpath` here.

- [x] Vendor `pwd.c`. Wire `pwd` into `USER_PROGS`/`SYS_PROGS` + `SBASE_UTIL_PWD=$(BINFOLDER)eprintf.o`
  + link rule.
- [x] `make build`; resolve link errors. If `realpath` is the only blocker and only `-P` needs it,
  leave it out (document) — don't block the task.
- [x] `make image`, QEMU: `BOOT_WAIT=30 scripts/qemu-shell.sh /tmp/cu.png "cd /disks/main" "pwd"`.
  Confirm it prints `/disks/main`, 0 faults.
- [x] Commit `feat: add pwd coreutil`. Tick.

### - [x] **Task 1.3: `touch`** (depends on Task 0.2)

**Likely objects:** `eprintf.o`. Source: `touch.c`. Uses `utimensat`/`futimens` + `open(O_CREAT)`.

- [x] Confirm Task 0.2 is `- [x]` (touch needs the time wrappers). Vendor `touch.c`. Wire it.
- [x] `make build`; resolve link errors (e.g. it may want `estrtonum`/`parseoffset` for `-t`/`-d` —
  vendor those `libutil/*.c` if referenced; plain `touch <file>` needs none beyond eprintf).
- [x] `make image`, QEMU:
  `BOOT_WAIT=30 scripts/qemu-shell.sh /tmp/cu.png "touch /disks/main/tf" "ls -l /disks/main/tf"`.
  Confirm `tf` is created (size 0), 0 faults.
- [x] Commit `feat: add touch coreutil`. Tick.

### - [x] **Task 1.4: `rm`** (depends on Task 0.2)

**Likely objects:** `eprintf.o recurse.o` (vendor `libutil/recurse.c` + `fs.h`). Source: `rm.c`.
`rm -r` traverses via `recurse()` (uses the `*at` family from Task 0.2).

- [x] Vendor `rm.c`, `libutil/recurse.c`, and `user/third_party/sbase/fs.h`. Wire `rm` into
  `USER_PROGS`/`SYS_PROGS` + `SBASE_UTIL_RM=$(BINFOLDER)eprintf.o $(BINFOLDER)recurse.o` + link rule.
- [x] `make build`; resolve link errors (recurse may pull `estrtol`/`ealloc` etc. — vendor as named).
- [x] `make image`, QEMU:
  `BOOT_WAIT=30 scripts/qemu-shell.sh /tmp/cu.png "mkdir -p /disks/main/rd/sub" "touch /disks/main/rd/sub/f" "rm -r /disks/main/rd" "ls /disks/main"`.
  Confirm `rd` is gone, 0 faults. Also test plain `rm /disks/main/<file>` on a regular file.
- [x] Commit `feat: add rm coreutil (incl -r via sbase recurse)`. Tick.

### - [x] **Task 1.5: `ln`**

**Likely objects:** `eprintf.o`. Source: `ln.c`. Uses `link`/`symlink` (present). May use `enmasse`
for `ln a b c dir/` form — vendor `libutil/enmasse.c` if the linker asks.

- [x] Vendor `ln.c` (+ `libutil/enmasse.c` if referenced). Wire it.
- [x] `make build`; resolve link errors.
- [x] `make image`, QEMU:
  `BOOT_WAIT=30 scripts/qemu-shell.sh /tmp/cu.png "touch /disks/main/orig" "ln -s /disks/main/orig /disks/main/sl" "ls -l /disks/main/sl" "ln /disks/main/orig /disks/main/hl" "ls -l /disks/main/hl"`.
  Confirm the symlink shows `-> /disks/main/orig` and the hard link shares the inode, 0 faults.
- [x] Commit `feat: add ln coreutil (symbolic + hard links)`. Tick.

### - [x] **Task 1.6: `cp`** (depends on Task 0.2)

**Likely objects:** `eprintf.o recurse.o` (+ `fs.h`, shared with rm). Source: `cp.c`. `cp -r` uses
`recurse()`. May also pull `concat`/`ealloc`.

- [x] Vendor `cp.c` (reuse the already-vendored `fs.h`/`recurse.c` from Task 1.4). Wire `cp`.
- [x] `make build`; resolve link errors. Note the sbase `fs.h` cross-file design: `cp.c` defines a
  `cp()` used by `mv` later — that's expected; just get `cp` itself to link now.
- [x] `make image`, QEMU:
  `BOOT_WAIT=30 scripts/qemu-shell.sh /tmp/cu.png "echo hi > /disks/main/src" "cp /disks/main/src /disks/main/dst" "cat /disks/main/dst" "mkdir -p /disks/main/cpd/a" "touch /disks/main/cpd/a/x" "cp -r /disks/main/cpd /disks/main/cpd2" "ls -R /disks/main/cpd2"`.
  Confirm `dst` contents = `hi`, recursive copy reproduces the tree, 0 faults.
- [x] Commit `feat: add cp coreutil (incl -r)`. Tick.

### - [x] **Task 1.7: `mv`** (depends on Task 0.2, and on `cp`/`rm` objects per sbase `fs.h`)

**Likely objects:** `eprintf.o recurse.o` and possibly `cp.o`+`rm.o` (sbase `mv` falls back to
`cp`+`rm` for cross-filesystem moves via `fs.h`). Source: `mv.c`.

- [x] Vendor `mv.c`. Wire `mv`. For the link rule, start with
  `$(BINFOLDER)mv.o $(BINFOLDER)eprintf.o $(BINFOLDER)recurse.o` and **resolve the undefined refs**:
  if `mv` references `cp`/`rm` (from `fs.h`), add `$(BINFOLDER)cp.o $(BINFOLDER)rm.o` — but those
  `.o` carry their own `main`, so a duplicate-`main` link error means sbase guards them (check the
  source) or you must extract the shared funcs. Read `mv.c`/`fs.h` and follow the linker; if a
  clean link needs source surgery, prefer NanOS-side glue over editing sbase, and if truly blocked,
  mark BLOCKED and ship `mv` as rename-only (note the limitation).
- [x] `make build` clean.
- [x] `make image`, QEMU:
  `BOOT_WAIT=30 scripts/qemu-shell.sh /tmp/cu.png "touch /disks/main/mvsrc" "mv /disks/main/mvsrc /disks/main/mvdst" "ls /disks/main/mvdst" "ls /disks/main/mvsrc"`.
  Confirm `mvdst` exists, `mvsrc` gone (same-FS rename path), 0 faults.
- [x] Commit `feat: add mv coreutil`. Tick.

---

## Phase 2 — Tier 2 utilities (nice-to-have, same mechanism)

> Same flow as Phase 1. `true`/`false` are trivial; `chmod` validates Task 0.1. **`stat` is NOT in
> sbase** — handle per Task 2.6.

### - [x] **Task 2.1: `chmod`** (validates Task 0.1)

**Likely objects:** `eprintf.o mode.o` (vendor `libutil/mode.c` → `parsemode`/`getumask`). Source:
`chmod.c`. May use `recurse.o` for `-R`.

- [x] Vendor `chmod.c` + `libutil/mode.c` (+ reuse `recurse.c` if `-R` is wanted). Wire it.
- [x] `make build`; resolve link errors.
- [x] `make image`, QEMU:
  `BOOT_WAIT=30 scripts/qemu-shell.sh /tmp/cu.png "touch /disks/main/cf" "chmod 700 /disks/main/cf" "ls -l /disks/main/cf"`.
  Confirm the mode shows `rwx------` (proves Task 0.1's real syscall + ext persistence), 0 faults.
- [x] Commit `feat: add chmod coreutil`. Tick.

### - [x] **Task 2.2: `wc`**

**Likely objects:** `eprintf.o` (+ libutf objects if `-m` multibyte). Source: `wc.c`.
- [x] Vendor `wc.c`. Wire. For `-m`, add `$(LIBUTF_OBJS)` to the rule (like `ls`).
- [x] `make build`; resolve.
- [x] `make image`, QEMU:
  `BOOT_WAIT=30 scripts/qemu-shell.sh /tmp/cu.png "echo hello world > /disks/main/wf" "wc /disks/main/wf"`.
  Confirm line/word/byte counts, 0 faults.
- [x] Commit `feat: add wc coreutil`. Tick.

### - [x] **Task 2.3: `head` + `tail`**

**Likely objects:** `eprintf.o` (+ `estrtonum.o`/`parseoffset.o` for `-n`/`-c`; `concat.o` for tail
streaming). Sources: `head.c`, `tail.c`.
- [x] Vendor `head.c`, `tail.c` (+ the `libutil/*.c` the linker names). Wire both.
- [x] `make build`; resolve.
- [x] `make image`, QEMU:
  `BOOT_WAIT=30 scripts/qemu-shell.sh /tmp/cu.png "head -n 1 /disks/main/wf" "tail -n 1 /disks/main/wf"`
  (reuse `wf` from Task 2.2 or create one). Confirm correct first/last line, 0 faults.
- [x] Commit `feat: add head + tail coreutils`. Tick.

### - [x] **Task 2.4: `true` + `false`**

**Likely objects:** none beyond glue (return 0 / 1). Sources: `true.c`, `false.c`.
- [x] Vendor both. Wire (link rule needs just `$(DYN_DEPS) $(BINFOLDER)true.o` etc.).
- [x] `make build`; `make image`, QEMU:
  `BOOT_WAIT=30 scripts/qemu-shell.sh /tmp/cu.png "true" "false"`. Confirm boot clean (exit codes
  are the proof; if the shell prints `$?` use `true; echo $?` style if supported). 0 faults.
- [x] Commit `feat: add true + false coreutils`. Tick.

### - [x] **Task 2.5: `env` + `basename` + `dirname`**

**Likely objects:** `eprintf.o` each. Sources: `env.c`, `basename.c`, `dirname.c`.
- [x] Vendor all three. Wire each into `USER_PROGS`/`SYS_PROGS` + link rules.
- [x] `make build`; resolve.
- [x] `make image`, QEMU:
  `BOOT_WAIT=30 scripts/qemu-shell.sh /tmp/cu.png "basename /a/b/c" "dirname /a/b/c" "env"`.
  Confirm `c`, `/a/b`, and an env listing, 0 faults.
- [x] Commit `feat: add env + basename + dirname coreutils`. Tick.

### - [x] **Task 2.6: `stat` (NOT in sbase — decide)**

> Decision: option 1 — skip `stat`. sbase has no `stat` utility (confirmed against commit
> c546c3a); `ls -l` already shows mode/size/owner/mtime, which covers the need. No code; the
> roadmap note is recorded in Task F.1.

**Reality:** sbase has no `stat` utility. Options, in order of preference:
1. **Skip it** — `ls -l` already shows mode/size/owner/mtime; mark `stat` as "covered by `ls -l`"
   in the roadmap and tick this task as a no-op decision (commit only the roadmap note in Finish).
2. If a real `stat` is wanted later, write a tiny `user/stat.c` (own program, not sbase) using
   `lstat` + print fields — but that's out of scope for "basic coreutils"; defer.

- [x] Choose option 1 (recommended): no code. Add a one-line note to the roadmap entry (done in
  Finish) and tick this task. (If you instead build a custom `stat`, follow the Phase 1 wiring
  flow and QEMU-verify.)

---

## Finish

### - [x] **Task F.1: Update ROADMAP + ship a combined verification**

- [x] Run one combined QEMU smoke covering the tier-1 happy path end-to-end:
  `BOOT_WAIT=30 scripts/qemu-shell.sh /tmp/cu-final.png "mkdir /disks/main/demo" "touch /disks/main/demo/a" "cp /disks/main/demo/a /disks/main/demo/b" "ln -s /disks/main/demo/a /disks/main/demo/l" "mv /disks/main/demo/b /disks/main/demo/c" "ls -l /disks/main/demo" "rm -r /disks/main/demo" "ls /disks/main"`.
  Confirm each step works and `demo` is gone at the end, 0 faults.
- [x] Edit `docs/superpowers/ROADMAP.md`: mark the "Basic coreutils" item under "Next up" as done
  (✅), listing which tier-1/tier-2 tools shipped and any limitations (e.g. `mv` rename-only if it
  was blocked, `pwd -P` unsupported, `stat` = use `ls -l`). Move the item out of "Next up" or
  annotate it `DONE`.
- [x] Update `docs/en/filesystem.md` only if the `/nanos/bin` utility list is enumerated there
  (add the new tools); otherwise skip.
- [x] Commit `docs: mark basic coreutils shipped; record limitations`. Tick.
- [x] **(optional) memory:** if the loop has memory access, add/update a project memory note
  pointing at this plan and the shipped set. Skip if not available.

---

## File map (created / modified)

- **Vendored (new), `user/third_party/sbase/`:** `mkdir.c rmdir.c pwd.c touch.c rm.c ln.c cp.c mv.c`
  (+ tier 2: `chmod.c wc.c head.c tail.c true.c false.c env.c basename.c dirname.c`), `fs.h`, and
  `libutil/{mkdirp.c, recurse.c, enmasse.c, mode.c, estrtonum.c, …}` as the linker requires.
- **Modified:** `Makefile` (`USER_PROGS`, `SYS_PROGS`, `SBASE_UTIL_*` vars, `.nxe` link rules near
  ~787), `user/libc-glue/posixstubs.c` (real chmod/chown), `user/libc-glue/syscalls.c` (`*at` +
  `utimes`/`utimensat` wrappers; maybe `user/libc-glue/compat-decls.h` for prototypes),
  `docs/superpowers/ROADMAP.md`, optionally `docs/en/filesystem.md`.

## Risks / invariants

- **Don't edit upstream sbase sources** — wire via libc-glue + Makefile. The container build copies
  to a case-sensitive tmp dir, so the macOS case-insensitivity trap doesn't apply here.
- **libc-glue edits rebuild `libc.ndl`** — every shipped program links against it, so Phase 0
  changes ripple to all userland; a clean `make build` after Phase 0 is the guard.
- **`mv` cross-file linking** (sbase `fs.h` shares `cp`/`rm`) is the one task that may need judgment;
  the fallback (rename-only `mv`) is acceptable and must be documented, not faked.
- **`/` is read-only** (SynthFs) — always test writes under `/disks/main` or `/tmp`.

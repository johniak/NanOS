# NanOS — nanoshell + verbatim sbase `cat`/`ls` on a ported libc

_Design spec. Date: 2026-06-07. Branch: `dockerized-build`._

## Goal

Add an interactive shell and two **real third-party coreutils** to NanOS, exercising
the existing app-loading + syscall stack:

- **`nanoshell`** (binary/command name **`nsh`**, `/bin/nsh.nxe`) — our own ~100-line
  interactive shell (prompt → read line → parse → run command), the kernel's first boot
  target instead of the `init ×2` demo.
- **`cat`, `ls`** — **unmodified upstream sbase sources** (`cat.c`, `ls.c`), compiled
  against a ported libc. Full fidelity: `ls.c` keeps its libutf dependency; `ls -l`
  shows real mode/size/owner data.

This is a fun milestone that also lays the groundwork for the roadmap's future
userland libc / `.ndl` story.

## Settled decisions (from brainstorming)

1. **Invocation:** build a tiny interactive shell (`nanoshell`), not a hardcoded demo
   and not argv-only. No off-the-shelf tiny shell works (xv6 `sh.c`, `lsh` all need
   fork/exec/wait + libc, which we lack); `nanoshell` is ours, with xv6 `sh.c` as a
   structural reference only.
2. **Process model:** synchronous **`SYS_spawn`** — the kernel runs the child `.nxe`
   to completion in its own address space (reusing Stage-2 ring-3 machinery, nested),
   returns the exit code; the shell blocks meanwhile. Stays single-process, **no
   scheduler**. cat/ls are real separate `.nxe` programs.
3. **cat/ls fidelity:** **full** — `cat.c` and `ls.c` are copied **verbatim** from
   sbase; `ls.c` keeps `utf.h`/libutf (Unicode tables copied too). Everything the OS
   cannot know is isolated in the libc/stub layer, never by editing the app sources.
4. **libc base:** **port picolibc** (newlib lineage) as a *separate static library*
   built against the existing `i686-elf` cross-gcc — **no toolchain rebuild**. We
   write only the thin OS porting layer (syscall stubs + `sbrk`) plus the few POSIX
   bits newlib/picolibc omits (`pwd`/`grp`, a `getdents` backend).
5. **v1 scope:** line editing (backspace), builtins `exit`/`echo`, `ls -l`
   (type + size + real mode/owner). `cat` reads stdin when given no file args.

## Licensing

- sbase (`cat.c`, `ls.c`, `arg.h`, `util.h`, `libutil/*`, `libutf/*`, `utf.h`) is
  **ISC/MIT**. Keep every upstream `LICENSE`/header verbatim; vendor under
  `user/third_party/sbase/` with the sbase `LICENSE` file alongside.
- picolibc is **BSD/newlib-license**; vendored or fetched in the Docker build with its
  license retained under `user/third_party/picolibc/`.
- These third-party trees are **excluded from the coverage gate** (like
  `AtaBlockDevice`) and from `make check-arch` (they are userland, not MI kernel code).
- No NanOS commit/PR mentions Claude (project rule).

## Architecture — four layers

```
Layer 4  apps        nanoshell.c (ours)   |  sbase cat.c / ls.c (verbatim)
Layer 3  libutf      sbase libutf + utf.h (verbatim, for ls full fidelity)
Layer 2  libc        picolibc (printf/stdio/qsort/malloc/string/time)
                     + porting layer: syscall stubs, _sbrk, pwd/grp, getdents backend
Layer 1  kernel      keyboard buffer + cooked stdin, SYS_spawn, argv on stack,
                     SYS_stat + ext metadata plumbing, user heap window
```

Boot → kernel `execProgram("/bin/nsh.nxe")` (ring 3, own address space, exactly
the Stage-2 path). The shell loops: print prompt → `read(0,…)` a line → parse argv →
builtin or `spawn`.

### Layer 1 — Kernel

**1a. Keyboard buffer + blocking cooked stdin.**
- The PS/2 IRQ1 handler stops echoing directly and instead pushes ASCII into a
  fixed-size **line-discipline buffer** (cooked mode): printable chars are echoed and
  appended; **Backspace** erases the last char and emits `\b \b`; **Enter** commits the
  line (echo `\n`, mark line ready). Ctrl-D on an empty line → EOF marker.
- `Syscalls::read(fd=0,…)` blocks until a committed line (or EOF) is available, copying
  up to `n` bytes and consuming them. Blocking = `arch::cpuEnableInterrupts()` + `hlt`
  loop (single process; the keyboard IRQ fills the buffer). A new arch contract
  `arch/console.h` (or `arch/input.h`) exposes the buffer to the MI syscall layer.
- This gives both `nanoshell` and `cat <stdin>` line editing for free (Unix tty model:
  the terminal driver does cooked mode, not the app).

**1b. `SYS_spawn(path, argv)` — synchronous nested exec.**
- New **NanOS-owned** syscall number, deliberately outside the Linux i386 range so it
  never collides with a Linux number we might add later: **`SYS_spawn = 500`**
  (documented in `SyscallNr.h`). Signature: `int spawn(const char* path, char* const
  argv[])` → child exit code, or `-ENOENT` etc.
- Kernel side reuses `arch::execUserImage`, made **re-entrant**:
  - **Save/restore the global `g_userCtx`** (the single ring-3 longjmp context) on the
    kernel stack around the nested run — the child's `enterUser` overwrites it; restore
    the parent's afterward.
  - **Per-depth kernel stack for TSS `esp0`.** A second kernel stack (depth ≤ 2:
    shell → command). Before running the child, `setKernelStack(childKstackTop)`;
    restore the parent's `esp0` after. (Otherwise the child's first `int 0x80` resets
    `esp` to the top of the *shared* kernel stack and clobbers the in-flight spawn
    frames.)
  - **CR3:** capture the parent's CR3 on entry; `execUserImage` switches to the child
    space and `userExit` switches back to the kernel dir; before `iret`-resuming the
    parent (ring 3) we `loadCr3(parentCr3)`.
  - The shell is NOT exited — after the child returns, the spawn syscall returns
    normally (writes child code to `regs->eax`, `iret` back to the shell). Only the
    child path uses longjmp.

**1c. argv on the user stack (SysV i386 ABI).**
- `spawn` copies the parent's argv strings into the child address space and builds, at
  the top of the child stack (`0x500000`), the layout: strings, then a NULL-terminated
  `argv[]` pointer array, then `argc`, with `esp` pointing at `argc`. The strings and
  pointers use **child-space virtual addresses** (the kernel writes them while the
  child space is mapped / via its physical frames).
- `user/crt0.S` changes: instead of a fixed `esp`, read `argc`/`argv` from the stack
  the kernel prepared and pass them to `main(argc, argv)`. (The kernel's `iret` already
  sets `esp` to the prepared stack top.)

**1d. `SYS_stat(path, struct stat*)` + ext metadata plumbing.**
- New `SYS_stat` (stat by path; we only have `fstat` by fd today). Fills a Linux-ish
  `struct stat` the picolibc headers expect.
- Extend the VFS `FileStat` and `ExtFilesystem` inode read to carry the fields the ext
  inode already stores: `mode`, `nlink (i_links_count)`, `uid`, `gid`, `size`,
  `mtime (i_mtime)`. Today `FileStat` exposes only `type` + `size`.
- `Syscalls::fstat`/`stat` map `FileStat` → `struct stat`. Fields the FS genuinely
  lacks stay zero/fixed. Result: `ls -l` shows **real** permissions, link count, size,
  and mtime.

**1e. User heap window.**
- `arch::execUserImage` maps a zeroed **heap region** for the child (e.g.
  `[roundUp(bssEnd) .. 0x4F0000)`, below the stack region `[0x4F0000,0x500000)`) as
  private USER frames. `_sbrk` hands out from `[heapStart, heapEnd)`; running past
  `heapEnd` returns `-1`/`ENOMEM`. Single fixed window is enough for `ls` (malloc +
  qsort over a directory's entries).

### Layer 2 — picolibc port

- **Build:** add a picolibc build step to `docker/Dockerfile` (`nanos-build`),
  targeting `i686-elf`, soft-float, 32-bit, built **against the already-built cross-gcc**
  (Meson cross-file). Produces `libc.a` + headers + `picolibc.specs`. Lands in a known
  prefix the userland link step references. (Trailing Docker layer so it doesn't
  invalidate the expensive toolchain cache.)
- **Porting layer** `user/libc-glue/syscalls.c` — implement the newlib syscall stubs
  over `int 0x80`: `_read _write _open _close _lseek _fstat _stat _exit _sbrk _isatty
  _getpid _kill`. Negative kernel return → set `errno`, return `-1`. This
  replaces most of today's `user/libnanos.c`.
- **POSIX bits newlib/picolibc omit — we add:**
  - `user/libc-glue/pwd_grp.c`: `getpwuid`/`getgrgid` returning fixed `root`/`wheel`
    (the FS has no name DB). Headers `<pwd.h>`/`<grp.h>` provided if picolibc lacks them.
  - `user/libc-glue/dirent_backend.c`: the `getdents`/`readdir` backend over our
    `SYS_getdents64` so picolibc's `opendir/readdir/closedir` work.
  - `time`/`localtime`/`strftime`: provided by picolibc; `_gettimeofday`/`time` stub
    returns a fixed epoch (no RTC) → `ls -l` shows a constant timestamp. (Acceptable;
    documented.)
- **crt0:** keep a small `user/crt0.S` that reads argc/argv (1c), calls
  `__libc_init_array`, `main`, then `exit` (picolibc-compatible startup).

### Layer 3 — sbase libutf

- Vendor `user/third_party/sbase/libutf/` + `utf.h` verbatim (rune.c, runetype tables,
  `chartorune`, `runetochar`, `isprintrune`). ISC, self-contained (only needs basic
  libc). Required because we chose full-fidelity `ls.c`.

### Layer 4 — applications

- `user/third_party/sbase/cat.c`, `ls.c` — **verbatim**. Plus the sbase support files
  they include: `arg.h`, `util.h`, and the `libutil` objects actually referenced:
  `eprintf weprintf enprintf` (xvprintf), `concat writeall`, `ereallocarray reallocarray
  estrdup`, `humansize`, `estrtonum strtonum`, `fshut`, `putword` (as pulled in).
  Vendor only what the two programs transitively need.
- `user/nsh.c` (the **`nsh`** shell) — **ours**, ~100 lines:
  - print prompt `nsh$ `; `read(0, line, …)` (cooked line from Layer 1).
  - split into argv on spaces.
  - builtins: `exit [code]` → return to kernel; `echo args…` → write to stdout.
  - else `spawn("/bin/" + argv0 + ".nxe", argv)`; on `-ENOENT` print
    `nanoshell: <cmd>: command not found`.
  - loop forever (until `exit`).

## Build changes (Makefile)

- Generalize the `_userland` target from one program to **N** programs: build
  `nsh.nxe`, `cat.nxe`, `ls.nxe` (each = crt0 + nxhdr + app/libutil objects,
  linked against picolibc `libc.a` + our glue, `objcopy -O binary`).
- `_image`: write all three to `/bin` (and keep/replace `init.nxe` per below).
- Kernel boot target: `Kernel::start` execs `/bin/nsh.nxe` instead of the
  `init ×2` demo. (The old `init.nxe` may stay as a buildable program but is no longer
  the boot target; its uncommitted `write(1,"test\n",5)` debug line is removed.)
- Coverage/check-arch: third-party userland trees excluded.

## Data flow (end to end)

`ls -l /boot` ⏎ →
kernel cooked-mode assembles the line →
`read(0)` in `nsh` returns it →
shell parses `argv = ["ls","-l","/boot"]` →
`spawn("/bin/ls.nxe", argv)` →
kernel: new address space, load `ls.nxe`, copy argv onto child stack, set child kstack,
`iret` to ring 3 →
`ls` calls `opendir/readdir/stat/printf` (picolibc) →
those call our `int 0x80` stubs → VFS/ext → screen →
`ls` `exit` → longjmp → `spawn` returns code → shell restores its ctx → prompt.

## Error handling

- Stubs translate negative kernel returns to `errno` + `-1`; sbase `weprintf("open
  %s:", …)` prints them (e.g. missing file → `ENOENT`).
- Unknown command → `spawn` returns `-ENOENT` → shell prints `command not found`.
- Writes to a file → `-EROFS` (FS is read-only); cat writes to stdout (console), so it
  is unaffected.
- Heap exhaustion → `_sbrk` returns `-1` → newlib `malloc` returns NULL → sbase
  `ereallocarray` calls `eprintf` and exits non-zero (child only; shell survives).

## Testing (TDD, host doctest, ≥90% MI gate)

Host-testable (pure MI logic, no hardware):
- **argv builder** — extract `buildUserStack(argv, stackTop, writeByte)` as a pure
  function over an injected memory sink; assert layout (argc, argv[] NULL-terminated,
  strings, esp target) and pointer values for several argv sets.
- **cooked line discipline** — extract the line buffer (append/backspace/commit/EOF)
  as a pure unit; assert backspace at empty is a no-op, commit boundaries, overflow.
- **SYS_stat mapping** — `FileStat` → `struct stat` field mapping (mode/size/nlink/
  uid/gid/mtime), dir vs file mode bits.
- **getdents → dirent** backend translation (reuse existing getdents64 fixture).

QEMU headless (per CLAUDE.md screendump + `-d int`):
- boots into `nsh$` prompt; `ls -l /boot` shows real entries with mode/size; `cat
  /boot/grub/grub.cfg` prints it; `echo hi`; `exit` returns cleanly. No `v=08/0d/0e`
  faults during normal use. Ring-3 proof: `int 0x80` traps at `cpl=3` for both shell
  and spawned child; nested spawn returns and the shell resumes.

Excluded from the coverage gate (third-party / hardware): picolibc, sbase, libutf,
the keyboard/ring-3/spawn arch glue (QEMU-verified, like `AtaBlockDevice`).

## Risks / gotchas

- **Nested ring-3 kernel stack** — the child's traps land on TSS `esp0`; must point it
  at a *separate* stack during the child's run or it clobbers the in-flight spawn
  frames. (Per-depth stack, depth ≤ 2.)
- **Global `g_userCtx`** — single longjmp context; save/restore around nested
  `execUserImage`.
- **CR3 discipline** — restore the parent's CR3 before `iret`-resuming the shell.
- **argv lives in two address spaces** — copy parent strings into the child space; the
  argv pointers must be child-space VAs.
- **picolibc build cache** — keep the picolibc build in a trailing Docker layer so it
  never invalidates the 20–40-min cross-toolchain cache.
- **No RTC** — `ls -l` timestamps are a fixed epoch; documented, not a bug.
- **`mmuDestroyAddressSpace` still leaks** (pre-existing) — each spawned child leaks its
  page tables; fine for a hobby OS, noted for the eventual selective-free TODO.
- **picolibc may lack `<pwd.h>`/`<grp.h>`/getdents backend** — we provide them; verify
  during the build-integration step.

## Out of scope (v1)

- Pipes, redirection, job control, background (`&`), globbing, env vars, `$PATH`
  (commands resolved only under `/bin`).
- A current working directory / relative paths. **Decision: no cwd — all paths are
  absolute.** sbase `ls` with no argument defaults to `.`, which we have no notion of;
  to keep `ls.c` verbatim, the shell rewrites a bare `ls` into `ls /` before spawning.
  `cat` likewise requires explicit absolute paths.
- Writable filesystem, real ownership/timestamps.
- Multiprocessing / scheduler (roadmap Stage 3) — `spawn` is deliberately synchronous.

## Build order (one commit each; build + test + check-arch green per step)

1. Kernel: keyboard buffer + blocking cooked stdin (+ host test for the line buffer).
2. Kernel: argv-on-stack builder (+ host test) and crt0 reading argc/argv (verify with
   the existing `init.nxe` still running, now receiving argv).
3. Kernel: `SYS_spawn` re-entrant `execUserImage` (save/restore g_userCtx, per-depth
   kstack, CR3) — verify by spawning `init.nxe` from a tiny stub.
4. Kernel: `SYS_stat` + ext metadata plumbing through VFS/FileStat (+ host test).
5. Docker/build: picolibc port + porting-layer stubs + pwd/grp + getdents backend;
   generalize `_userland` to multiple programs.
6. Vendor sbase libutf + `cat.c` verbatim; build `cat.nxe`; QEMU-cat a file.
7. Vendor sbase `ls.c` verbatim (full libutf); build `ls.nxe`; QEMU `ls -l /boot`.
8. Write `nsh.c` (the nanoshell); make it the boot target; QEMU full demo (prompt, ls,
   cat, echo, exit). Update ROADMAP.md (shell + coreutils done).
```

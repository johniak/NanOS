# nanoshell (`nsh`) + verbatim sbase `cat`/`ls` on ported picolibc — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Boot NanOS into an interactive shell (`nsh`) that runs unmodified upstream sbase `cat` and `ls` (`ls -l` with real metadata) as separate `.nxe` programs, on a ported picolibc.

**Architecture:** Kernel gains four primitives — cooked blocking stdin, `SYS_spawn` (synchronous nested ring-3 exec), argv-on-user-stack, and `SYS_stat` + ext metadata plumbing. Userland gains a ported picolibc (built standalone against the existing `i686-elf` cross-gcc) plus a thin syscall/`sbrk`/pwd-grp/getdents glue layer. The three programs (`nsh` ours; `cat`/`ls` verbatim sbase + libutf) link against it.

**Tech Stack:** i686-elf cross-gcc (Docker `nanos-build`), nasm, picolibc (Meson cross build), sbase (ISC), doctest host tests (Docker `nanos-test`), QEMU i386.

**Spec:** `docs/superpowers/specs/2026-06-07-nanoshell-coreutils-design.md`

**Branch:** `dockerized-build`. **Commit rule:** the user commits/pushes on request — each task ends with a *prepared* commit (staged + message), run only when the user approves a batch. No Claude attribution in messages.

**Conventions reminder:**
- `make build` / `make image` / `make run` go through Docker; `make test` / `make coverage` use `nanos-test`; `make check-arch` guards the MI boundary. Run `make test` + `make check-arch` after every kernel task; `make build` after userland tasks; QEMU at phase ends.
- Host tests must not include the freestanding `<string.h>` (use libc via shims); registered devices/types need static/heap lifetime.
- MI dirs (`init kernel mm fs lib drivers`) may only reach the arch via `<arch/...>`; x86 internals stay under `arch/x86/`.

---

## File Structure (what gets created / modified)

**Kernel — MI:**
- `kernel/SyscallNr.h` (modify) — add `SYS_spawn 500`, `SYS_stat 106`.
- `kernel/Syscall.h` / `Syscall.cpp` (modify) — blocking `read(0)` via an input contract; `stat(path,*)`; extend `LinuxStat`; a `spawn` hook field.
- `kernel/SyscallDispatch.cpp` (modify) — dispatch `SYS_spawn`, `SYS_stat`.
- `kernel/LineDiscipline.h` / `.cpp` (create) — pure cooked-mode line buffer (host-tested).
- `kernel/UserStack.h` / `.cpp` (create) — pure argv/stack-image builder (host-tested).
- `kernel/Exec.cpp` / `Exec.h` (modify) — `execProgram` passes argv; add `spawnProgram`.
- `fs/Vfs.h` (modify) — extend `FileStat` (mode/nlink/uid/gid/mtime).
- `fs/ExtFilesystem.cpp` (modify) — fill the new `FileStat` fields from the inode.

**Kernel — arch/x86:**
- `arch/include/arch/input.h` (create) — MI/MD input contract (`inputGetLineByte`/blocking).
- `arch/x86/drivers/Keyboard.cpp` (modify) — IRQ1 feeds the line discipline instead of echoing raw.
- `arch/x86/drivers/input_x86.cpp` (create) — implements `<arch/input.h>` over the buffer.
- `arch/include/arch/usermode.h` (modify) — `execUserImage`/`enterUser` take argv + heap.
- `arch/x86/cpu/usermode_x86.cpp` (modify) — re-entrant spawn: save/restore `g_userCtx`, per-depth kstack, CR3, push argv, map heap.
- `arch/x86/cpu/cpu_x86.cpp` (modify) — second kernel stack + accessor for spawn depth.
- `arch/x86/cpu/Gdt.*` — already has `setKernelStack` (reused).

**Userland:**
- `user/third_party/sbase/` (create) — vendored `cat.c`, `ls.c`, `arg.h`, `util.h`, `compat.h`, used `libutil/*`, `libutf/*`, `utf.h`, `LICENSE`.
- `user/third_party/picolibc/` (create) — picolibc source/build artifacts + `LICENSE` (or fetched in Docker).
- `user/libc-glue/syscalls.c` (create) — newlib syscall stubs over `int 0x80`.
- `user/libc-glue/pwd_grp.c` + headers (create) — `getpwuid`/`getgrgid` stubs.
- `user/libc-glue/dirent_backend.c` (create) — `getdents` backend over `SYS_getdents64`.
- `user/crt0.S` (modify) — read argc/argv, run `__libc_init_array`, `main`, `exit`.
- `user/nxhdr.c`, `user/nx.ld` (reuse; one per program via the generalized build).
- `user/nsh.c` (create) — the shell.
- `user/init.c` (modify/retire) — drop the `test\n` debug line; no longer the boot target.

**Build / docker:**
- `docker/Dockerfile` (modify) — trailing layer building picolibc for `i686-elf`.
- `Makefile` (modify) — generalize `_userland` to N programs; write all to `/bin`; exclude third-party from coverage.
- `arch/x86/arch.mk` (modify) — new arch objects (`input_x86.o`).
- `kernel/Kernel.cpp` (modify) — boot target `→ /bin/nsh.nxe`.
- Tests: `tests/test_linediscipline.cpp`, `tests/test_userstack.cpp`, `tests/test_statmap.cpp` (create); `Makefile` `TEST_MODULES`/`COV_PATTERNS` (modify).

---

## Phase 1 — Cooked blocking stdin (keyboard line discipline)

Goal: typing a line (with backspace + Enter) is captured in a buffer; `read(0,…)` blocks until a line is ready and returns it. Pure logic is host-tested; the IRQ/console wiring is QEMU-verified.

### Task 1.1: LineDiscipline pure unit (host-tested)

**Files:**
- Create: `kernel/LineDiscipline.h`, `kernel/LineDiscipline.cpp`
- Test: `tests/test_linediscipline.cpp`
- Modify: `Makefile` (`TEST_MODULES`, `COV_PATTERNS`)

- [ ] **Step 1: Write the failing test**

```cpp
// tests/test_linediscipline.cpp
#include "doctest.h"
#include "LineDiscipline.h"
using kernel::LineDiscipline;

TEST_CASE("printable chars accumulate; commit on newline") {
    LineDiscipline ld;
    char echo[64]; int en = 0;
    auto sink = [&](char c){ echo[en++] = c; };
    for (const char* p = "ab"; *p; p++) ld.push(*p, sink);
    CHECK(!ld.lineReady());
    ld.push('\n', sink);
    CHECK(ld.lineReady());
    char out[16]; int n = ld.takeLine(out, sizeof out);
    CHECK(n == 3);                       // "ab\n"
    CHECK(std::string(out, n) == "ab\n");
    CHECK(!ld.lineReady());              // consumed
    CHECK(std::string(echo, en) == "ab\n");
}

TEST_CASE("backspace erases last char and echoes \\b \\b; no-op when empty") {
    LineDiscipline ld;
    char echo[64]; int en = 0;
    auto sink = [&](char c){ echo[en++] = c; };
    ld.push('\b', sink);                 // empty: ignored
    CHECK(en == 0);
    ld.push('x', sink); ld.push('\b', sink);
    ld.push('\n', sink);
    char out[16]; int n = ld.takeLine(out, sizeof out);
    CHECK(n == 1);                       // just "\n"
    CHECK(out[0] == '\n');
    // echo: 'x', then BS SP BS, then '\n'
    CHECK(std::string(echo, en) == "x\b \b\n");
}

TEST_CASE("overflow drops chars past capacity but still commits") {
    LineDiscipline ld;
    auto sink = [](char){};
    for (int i = 0; i < LineDiscipline::CAP + 50; i++) ld.push('a', sink);
    ld.push('\n', sink);
    char out[LineDiscipline::CAP + 8];
    int n = ld.takeLine(out, sizeof out);
    CHECK(n == LineDiscipline::CAP);     // capacity incl. the newline slot
}

TEST_CASE("ctrl-D on empty line signals EOF (zero-length line ready)") {
    LineDiscipline ld;
    auto sink = [](char){};
    ld.push(4 /*EOT*/, sink);
    CHECK(ld.lineReady());
    char out[8];
    CHECK(ld.takeLine(out, sizeof out) == 0);
}
```

- [ ] **Step 2: Run it, confirm it fails to compile** — `make test` → expected: `LineDiscipline.h: No such file`.

- [ ] **Step 3: Write `kernel/LineDiscipline.h`**

```cpp
/*
 * LineDiscipline.h — cooked-mode terminal line buffer (machine-independent).
 *
 * Accumulates a line: printable chars are appended and echoed; Backspace erases
 * the last char (echo "\b \b"); Enter commits the line (echo "\n"); Ctrl-D (EOT)
 * on an empty line commits a zero-length line = EOF. takeLine() consumes a
 * committed line. The echo sink is injected so this is host-testable.
 */
#pragma once

namespace kernel {

class LineDiscipline {
public:
    static const int CAP = 256;   // max chars per line incl. trailing '\n'

    LineDiscipline() : m_len(0), m_ready(false), m_readyLen(0) {}

    // Feed one input char; `echo` is called for each char to display.
    template <class Echo>
    void push(char c, Echo echo) {
        if (m_ready) return;                 // wait for takeLine
        if (c == '\b' || c == 127) {         // backspace / DEL
            if (m_len > 0) { m_len--; echo('\b'); echo(' '); echo('\b'); }
            return;
        }
        if (c == 4) {                        // EOT (Ctrl-D): commit current line as-is
            commit(false);
            return;
        }
        if (c == '\n' || c == '\r') {
            if (m_len < CAP) m_buf[m_len++] = '\n';
            echo('\n');
            commit(true);
            return;
        }
        if (m_len < CAP) { m_buf[m_len++] = c; echo(c); }
        // else: overflow — drop silently (still commits on Enter)
    }

    bool lineReady() const { return m_ready; }

    // Copy the committed line into out (up to n). Returns its length, clears ready.
    int takeLine(char* out, int n) {
        int k = m_readyLen < n ? m_readyLen : n;
        for (int i = 0; i < k; i++) out[i] = m_buf[i];
        m_ready = false; m_readyLen = 0; m_len = 0;
        return k;
    }

private:
    void commit(bool /*hadNewline*/) { m_readyLen = m_len; m_ready = true; }
    char m_buf[CAP];
    int  m_len;
    bool m_ready;
    int  m_readyLen;
};

}  // namespace kernel
```

(Header-only template for the echo sink; `LineDiscipline.cpp` is an empty TU that just includes the header so the Makefile object rule has a source — or skip the .cpp and keep header-only. Use header-only: no `.cpp`.)

- [ ] **Step 4: Wire the test into the Makefile**

Modify `Makefile`: add `tests/test_linediscipline.cpp` to the doctest test sources list (find where `tests/test_*.cpp` are enumerated for `_test`). LineDiscipline is header-only, so nothing to add to `TEST_MODULES`/`COV_PATTERNS` — but to keep it under the gate, add a one-line `.cpp` `kernel/LineDiscipline.cpp` (`#include "LineDiscipline.h"`) and add `kernel/LineDiscipline.cpp` to `TEST_MODULES` + a `COV_PATTERNS` entry.

- [ ] **Step 5: Run tests, confirm pass** — `make test` → expected: 4 new cases PASS, coverage gate still ≥90%.

- [ ] **Step 6: Prepare commit**

```bash
git add kernel/LineDiscipline.h kernel/LineDiscipline.cpp tests/test_linediscipline.cpp Makefile
# message: "feat: cooked-mode line discipline (host-tested)"
```

### Task 1.2: arch input contract + keyboard wiring + blocking read

**Files:**
- Create: `arch/include/arch/input.h`, `arch/x86/drivers/input_x86.cpp`
- Modify: `arch/x86/drivers/Keyboard.cpp`, `arch/x86/arch.mk`, `kernel/Syscall.cpp`, `kernel/Syscall.h`

- [ ] **Step 1: Create `arch/include/arch/input.h`**

```cpp
/*
 * arch/input.h — MI/MD contract for blocking console input.
 *
 * The arch keyboard driver fills a cooked-mode line buffer (LineDiscipline). MI
 * code (the read(0) syscall) blocks here until a full line is available, then
 * copies it out. Blocking is arch-specific (x86: sti + hlt while the IRQ fills
 * the buffer).
 */
#pragma once

namespace arch {
// Block until a committed input line is available, then copy up to `n` bytes into
// `buf`. Returns the number of bytes copied (0 = EOF on Ctrl-D at empty line).
int inputReadLine(char* buf, unsigned n);
}
```

- [ ] **Step 2: Implement `arch/x86/drivers/input_x86.cpp`**

```cpp
#include <arch/input.h>
#include <arch/cpu.h>
#include "LineDiscipline.h"
#include "Console.h"

namespace kernel { extern LineDiscipline g_lineDiscipline; }  // defined in Keyboard.cpp

namespace arch {

int inputReadLine(char* buf, unsigned n) {
    while (!kernel::g_lineDiscipline.lineReady()) {
        arch::cpuEnableInterrupts();   // ensure IRQ1 can fire
        arch::cpuHalt();               // sleep until the next interrupt
    }
    return kernel::g_lineDiscipline.takeLine(buf, (int) n);
}

}  // namespace arch
```

- [ ] **Step 3: Rewrite the IRQ1 handler in `arch/x86/drivers/Keyboard.cpp`**

Replace `kb_handler`'s `Console::write(key)` with feeding the line discipline; add the global and skip key releases (scancode high bit set) and `KBD_SPECIAL`:

```cpp
#include "LineDiscipline.h"
namespace kernel {
LineDiscipline g_lineDiscipline;   // the system console line buffer

static void echoChar(char c) { Console::write(c); }

static void kb_handler(Registers* reg) {
    unsigned char sc = (unsigned char) IOPort::inb(0x60);
    if (sc & 0x80) return;                 // key release — ignore
    char key = scancode_ascii[sc];
    if (key == KBD_SPECIAL) return;        // non-text key
    g_lineDiscipline.push(key, echoChar);
}
}
```

(Backspace: the current `scancode_ascii` table maps scancode `0x0E` to `KBD_SPECIAL`. Change index `0x0E` from `KBD_SPECIAL` to `'\b'` so backspace reaches the discipline. Confirm the table index by counting — `0x0E` is the 15th entry, currently the `KBD_SPECIAL` right after `'='`.)

- [ ] **Step 4: Make `read(0)` block** — modify `kernel/Syscall.cpp` `Syscalls::read`:

```cpp
int Syscalls::read(int fd, void* buf, unsigned n) {
    if (!valid(fd))
        return -EBADF;
    if (fds[fd].isConsole)
        return arch::inputReadLine((char*) buf, n);   // blocking cooked line
    int r = vfs->read(fds[fd].path, n, fds[fd].offset, buf);
    if (r < 0) return 0;
    fds[fd].offset += (unsigned) r;
    return r;
}
```

Add `#include <arch/input.h>` to `kernel/Syscall.cpp`.

- [ ] **Step 5: Register the new arch object** — `arch/x86/arch.mk`: add `input_x86.o` to `ARCH_SOURCES`. Confirm `Keyboard.o` already present.

- [ ] **Step 6: Build + check-arch** — `make build` (expected: links clean), `make check-arch` (expected: "OK: MI layer is arch-clean." — `Syscall.cpp` uses only `<arch/input.h>`), `make test` (unchanged, still green).

- [ ] **Step 7: QEMU smoke** — temporarily add to `Kernel::start` (before the exec loop) a probe: `char b[64]; int n = arch::inputReadLine(b,64); Console::write(b...)`. Boot with `make run` (interactive, not headless), type `hi`⏎, confirm it echoes and prints back, backspace erases. Then remove the probe. (Interactive QEMU needed because this reads the keyboard.)

- [ ] **Step 8: Prepare commit**

```bash
git add arch/include/arch/input.h arch/x86/drivers/input_x86.cpp arch/x86/drivers/Keyboard.cpp arch/x86/arch.mk kernel/Syscall.cpp Makefile
# (arch.mk + Makefile if arch.mk lists objects there)
# message: "feat: blocking cooked stdin over the keyboard line discipline"
```

---

## Phase 2 — argv on the user stack

Goal: the kernel builds an `argc/argv[]/strings` image at the top of the child stack (SysV i386), and crt0 passes them to `main(argc,argv)`. Verified first with the existing single program.

### Task 2.1: UserStack pure builder (host-tested)

**Files:** Create `kernel/UserStack.h` + `kernel/UserStack.cpp`; Test `tests/test_userstack.cpp`; Modify `Makefile`.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/test_userstack.cpp
#include "doctest.h"
#include "UserStack.h"
#include <cstring>
using kernel::buildUserStack;

TEST_CASE("argv image: argc, NULL-terminated argv[], strings, esp at argc") {
    // Fake user stack as a host buffer; userBase is the VA the buffer represents.
    unsigned char buf[4096];
    const uint32_t userTop = 0x500000;
    const uint32_t userBase = userTop - sizeof(buf);
    auto va2host = [&](uint32_t va) -> unsigned char* { return buf + (va - userBase); };

    const char* argv[] = { "ls", "-l", "/boot", 0 };
    uint32_t esp = buildUserStack(userTop, argv, 3,
        [&](uint32_t va, const void* src, unsigned len){ memcpy(va2host(va), src, len); });

    // esp points at argc
    int argc = *(int*) va2host(esp);
    CHECK(argc == 3);
    uint32_t* uargv = (uint32_t*)(va2host(esp) + 4);
    CHECK(uargv[3] == 0);                                  // NULL-terminated
    CHECK(std::string((char*) va2host(uargv[0])) == "ls");
    CHECK(std::string((char*) va2host(uargv[1])) == "-l");
    CHECK(std::string((char*) va2host(uargv[2])) == "/boot");
    CHECK((esp & 0xF) == 0);                               // 16-byte aligned
}
```

- [ ] **Step 2: Run, confirm fail** — `make test` → `UserStack.h: No such file`.

- [ ] **Step 3: Implement `kernel/UserStack.h`**

```cpp
/*
 * UserStack.h — build a SysV-i386 argv image at the top of a user stack.
 *
 * Layout (high → low): argv strings, NUL-terminated; the argv[] pointer array
 * (argc entries + a NULL); then argc. esp ends 16-byte aligned, pointing at argc.
 * Writing is delegated (write(va, src, len)) so the same code serves the kernel
 * (write into the child's mapped frames) and host tests (write into a buffer).
 */
#pragma once
#include <stdint.h>

namespace kernel {

template <class WriteFn>
uint32_t buildUserStack(uint32_t userTop, const char* const* argv, int argc, WriteFn write) {
    uint32_t sp = userTop;
    uint32_t ptrs[64];
    // 1) strings, top-down
    for (int i = argc - 1; i >= 0; i--) {
        unsigned len = 0; while (argv[i][len]) len++; len++;   // incl NUL
        sp -= len;
        write(sp, argv[i], len);
        ptrs[i] = sp;
    }
    sp &= ~0xFu;                                   // keep things tidy
    // 2) argv[] array (argc pointers + NULL)
    uint32_t nullp = 0;
    sp -= 4; write(sp, &nullp, 4);
    for (int i = argc - 1; i >= 0; i--) { sp -= 4; write(sp, &ptrs[i], 4); }
    uint32_t argvAddr = sp;
    // 3) argc, then make esp 16-aligned with argc on top
    sp -= 4; write(sp, &argc, 4);
    // (we also stash argvAddr right above argc via the pointer array start;
    //  crt0 computes argv = esp + 4)
    (void) argvAddr;
    return sp;
}

}  // namespace kernel
```

(Header-only template; add a stub `kernel/UserStack.cpp` `#include "UserStack.h"` for the coverage object, as with LineDiscipline.)

- [ ] **Step 4: Wire into Makefile** (test source + `TEST_MODULES`/`COV_PATTERNS` for `kernel/UserStack.cpp`).

- [ ] **Step 5: Run, confirm pass** — `make test`.

- [ ] **Step 6: Prepare commit** — `feat: SysV-i386 argv user-stack builder (host-tested)`.

### Task 2.2: crt0 reads argc/argv; kernel pushes them

**Files:** Modify `user/crt0.S`, `arch/include/arch/usermode.h`, `arch/x86/cpu/usermode_x86.cpp`, `kernel/Exec.cpp`.

- [ ] **Step 1: Change `enterUser`/`execUserImage` signatures** in `arch/include/arch/usermode.h`:

```cpp
int execUserImage(uint32_t entry, uint32_t loadBase, uint32_t bssEnd,
                  const char* const* argv, int argc);
```

(`enterUser` keeps its signature; the argv image is written before it.)

- [ ] **Step 2: In `arch/x86/cpu/usermode_x86.cpp`**, after mapping the stack frames and before `enterUser`, build the argv image into the child stack. Because the child frames are mapped in `space` (not the current CR3), write through their *physical* addresses (the frames were `alloc()`'d and identity-mapped in the kernel dir). Capture each stack VA→frame mapping while mapping:

```cpp
#include "UserStack.h"
// ... in execUserImage, replace the stack-mapping loop to remember frames:
uint32_t stackFrames[16]; int sfi = 0;
for (uint32_t va = USER_STACK_BOT; va < USER_STACK_TOP; va += 0x1000) {
    uint32_t f = kernel::g_frames.alloc();
    memset((void*) f, 0, 0x1000);
    mmuMap(space, va, f, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
    stackFrames[sfi++] = f;            // index 0 = lowest VA
}
// VA→physical for the stack window:
auto stackPhys = [&](uint32_t va) -> uint32_t {
    uint32_t idx = (va - USER_STACK_BOT) >> 12;
    return stackFrames[idx] + (va & 0xFFF);
};
uint32_t esp = kernel::buildUserStack(USER_STACK_TOP, argv, argc,
    [&](uint32_t va, const void* src, unsigned len) {
        // argv image lives in the top page(s); copy byte-wise across the frame
        const unsigned char* s = (const unsigned char*) src;
        for (unsigned i = 0; i < len; i++)
            *(unsigned char*) stackPhys(va + i) = s[i];
    });
int rc = enterUser(entry, esp, space);
```

(`USER_STACK_TOP - USER_STACK_BOT = 0x10000` = 16 frames; `stackFrames[16]` fits. The argv strings for `cat`/`ls`/`nsh` are tiny — comfortably within the top frame.)

- [ ] **Step 3: Rewrite `user/crt0.S`** to consume argc/argv (esp points at argc; argv = esp+4):

```asm
[BITS 32]
[GLOBAL _start]
[EXTERN main]
[EXTERN exit]
[EXTERN __libc_init_array]

_start:
    mov ebp, 0
    mov eax, [esp]          ; argc
    lea edx, [esp + 4]      ; argv
    push edx                ; argv
    push eax                ; argc
    call __libc_init_array  ; run C++/picolibc init (no-op if none)
    mov eax, [esp]          ; reload argc
    mov edx, [esp + 4]      ; reload argv
    push edx
    push eax
    call main               ; main(argc, argv)
    add esp, 8
    push eax
    call exit
.hang:
    jmp .hang
```

(If `__libc_init_array` is unavailable until Phase 5, keep a weak stub or omit that call until picolibc lands; note this in the step. For now, before picolibc, define `__libc_init_array` as a no-op in a tiny `user/crt_init_stub.c` and remove it in Phase 5.)

- [ ] **Step 4: Update `kernel/Exec.cpp`** `execProgram` to pass argv. For the still-current `init.nxe`, pass a one-element argv:

```cpp
const char* argv[] = { path, 0 };
return arch::execUserImage(entry, h->loadBase, h->bssEnd, argv, 1);
```

- [ ] **Step 5: Build** — `make build` (expected: links). `make check-arch`, `make test` (UserStack test green).

- [ ] **Step 6: QEMU headless verify** — boot; `init.nxe` still cats grub.cfg and exits 0 ×2 (argv plumbed but init ignores it). Confirm no faults (`-d int`, no `v=0e`).

- [ ] **Step 7: Prepare commit** — `feat: pass argv to user programs via the SysV stack image`.

---

## Phase 3 — `SYS_spawn`: synchronous nested ring-3 exec

Goal: a program (eventually `nsh`) can run another `.nxe` to completion in its own address space and get the exit code. Re-entrant `execUserImage`: save/restore `g_userCtx`, switch to a second kernel stack for the child's traps, restore CR3 before resuming the parent.

### Task 3.1: Second kernel stack + spawn depth

**Files:** Modify `arch/x86/cpu/cpu_x86.cpp`, `arch/x86/cpu/usermode_x86.cpp`.

- [ ] **Step 1: Add a second kernel stack** in `cpu_x86.cpp` and expose both tops:

```cpp
unsigned char g_userKstack[8192];
unsigned char g_childKstack[8192];   // for the nested (spawned) child's traps
```

Add accessors in the `arch` namespace (declared in a small internal header or `usermode.h`):

```cpp
namespace arch {
unsigned kstackTop(int depth);   // depth 0 -> g_userKstack top, 1 -> g_childKstack top
void setKernelStack(unsigned esp0);   // forwards to g_gdt.setKernelStack
}
```

Implement in `cpu_x86.cpp` (where `g_gdt` is visible):

```cpp
unsigned arch::kstackTop(int depth) {
    return depth == 0 ? (unsigned)(g_userKstack + sizeof g_userKstack)
                      : (unsigned)(g_childKstack + sizeof g_childKstack);
}
void arch::setKernelStack(unsigned esp0) { g_gdt.setKernelStack(esp0); }
```

- [ ] **Step 2: Build** — `make build` (no behavior change yet). Commit prepared: `feat: second kernel stack for nested spawn`.

### Task 3.2: Re-entrant spawn in usermode_x86.cpp

**Files:** Modify `arch/x86/cpu/usermode_x86.cpp`, `arch/include/arch/usermode.h`.

- [ ] **Step 1: Track spawn depth and save/restore `g_userCtx`.** Add a file-scope `int g_depth = 0;`. Introduce `spawnUserImage` (called from the syscall path) that wraps the existing run with save/restore:

```cpp
// arch/include/arch/usermode.h
int spawnUserImage(uint32_t entry, uint32_t loadBase, uint32_t bssEnd,
                   const char* const* argv, int argc);
```

```cpp
// usermode_x86.cpp
int spawnUserImage(uint32_t entry, uint32_t loadBase, uint32_t bssEnd,
                   const char* const* argv, int argc) {
    NxJmp savedCtx = g_userCtx;                 // parent's longjmp context
    uint32_t parentCr3 = kernel::readCr3();     // parent address space
    unsigned parentEsp0 = arch::kstackTop(g_depth);   // parent's kernel stack top
    g_depth++;
    arch::setKernelStack(arch::kstackTop(g_depth));   // child traps land on a fresh stack
    bool savedExited = kernel::kernelSyscalls()->hasExited();
    int savedCode = kernel::kernelSyscalls()->code();
    kernel::kernelSyscalls()->resetForRun();

    int rc = execUserImage(entry, loadBase, bssEnd, argv, argc);  // runs child to exit

    // child done: restore everything for the parent
    g_depth--;
    arch::setKernelStack(parentEsp0);
    g_userCtx = savedCtx;
    kernel::loadCr3(parentCr3);                 // back to the parent's space
    // restore the parent's exit flag/code (it had not exited)
    if (savedExited) kernel::kernelSyscalls()->exit(savedCode);
    else kernel::kernelSyscalls()->resetForRun();
    return rc;
}
```

Add `kernel::readCr3()` to `PagingControl.h`/`.cpp` if not present (reads CR3). Add the `arch::kstackTop`/`setKernelStack` declarations to a header both files see.

- [ ] **Step 2: `execUserImage` exit must NOT escape to the parent's context.** It already calls `enterUser`, which `nx_setjmp`s `g_userCtx` itself, runs the child, and returns via `userExit`'s longjmp into *that* setjmp. Since `enterUser` re-`setjmp`s `g_userCtx` at child entry, the child's exit returns into `execUserImage`/`spawnUserImage`. Good — no change needed inside `enterUser`, but verify the order: `spawnUserImage` saves `g_userCtx` BEFORE calling `execUserImage` (which overwrites it), and restores AFTER. ✓

- [ ] **Step 3: Build** — `make build`. `make check-arch`.

- [ ] **Step 4: Wire a throwaway spawn test in the kernel:** temporarily, in `Kernel::start`, after the existing init runs, call (via a new MI hook — see Task 3.3) a spawn of `/bin/init.nxe` from *within* a parent that is itself running. Simplest interim check: have `init.c` (temporarily) call a new `spawn` syscall to run itself once (guarding against infinite recursion with an argv flag). Defer full proof to Phase 8 with `nsh`; here just confirm a single nested spawn returns and the outer program continues. QEMU `-d int`: expect `int 0x80` at `cpl=3` from both depths, the inner exits 0, the outer continues, no `v=08`.

- [ ] **Step 5: Prepare commit** — `feat: re-entrant SYS_spawn core (nested ring-3 exec)`.

### Task 3.3: `SYS_spawn` syscall + dispatch + glue

**Files:** Modify `kernel/SyscallNr.h`, `kernel/Syscall.h`/`.cpp`, `kernel/SyscallDispatch.cpp`, `kernel/Exec.h`/`.cpp`.

- [ ] **Step 1: Add the number** — `kernel/SyscallNr.h`:

```c
#define SYS_spawn 500   /* NanOS-private: run a child .nxe synchronously */
```

- [ ] **Step 2: Spawn entry point** in `kernel/Exec.cpp` (MI orchestration; arch does the ring work):

```cpp
// kernel/Exec.cpp
int spawnProgram(Vfs* vfs, const char* path, const char* const* argv, int argc) {
    String p((char*) path);
    FileStat st;
    if (vfs->stat(p, st) < 0) return -2;          // -ENOENT
    char* image = (char*) 0x400000;               // staging window (kernel identity)
    if (vfs->read(p, st.size, 0, image) < 0) return -1;
    NxHeader* h = (NxHeader*) image;
    unsigned entry = 0;
    int rc = NxeLoader::loadImage(image, h->bssEnd - h->loadBase, 0, &entry);
    if (rc < 0) return rc;
    return arch::spawnUserImage(entry, h->loadBase, h->bssEnd, argv, argc);
}
```

> **Staging-window note:** the parent program lives at VA `0x400000` in *its* space, but the syscall runs on the kernel stack with the parent's CR3 active. Writing the child image to VA `0x400000` would corrupt the parent. Fix: `spawnProgram` must switch to the kernel directory before staging (`kernel::loadCr3(arch::mmuKernelDirPhys())`), stage + load, then `spawnUserImage` switches into the child space. Add that `loadCr3` at the top of `spawnProgram` (and `spawnUserImage` already restores the parent CR3 at the end). Document this in the code comment.

Declare `int spawnProgram(Vfs*, const char*, const char* const*, int);` in `Exec.h`.

- [ ] **Step 3: Dispatch** — `kernel/SyscallDispatch.cpp`. `kernelSyscall` needs the `Vfs*` to spawn; store it in `installSyscalls` (file-scope `static Vfs* g_vfs;`) and copy argv from user memory. argv arrives as a user pointer to a NULL-terminated array of user `char*`. Since we're still in the parent's CR3 at dispatch time, the pointers are valid:

```cpp
case SYS_spawn: {
    const char* path = (const char*) a0;
    const char* const* uargv = (const char* const*) a1;
    int argc = 0; while (uargv && uargv[argc]) argc++;
    ret = spawnProgram(g_vfs, path, uargv, argc);
    break;
}
```

Add `case SYS_stat` placeholder later (Phase 4). Add `#include "Exec.h"` and the `g_vfs` capture in `installSyscalls`.

> **argv lifetime across CR3 switch:** `spawnProgram` switches to the kernel CR3 *before* it dereferences argv strings to copy them onto the child stack. So copy argv into a kernel-side buffer **before** the `loadCr3(kernelDir)` switch (while the parent space is still active). Implement: in `kernelSyscall`'s `SYS_spawn` case, deep-copy `path` + each `argv[i]` into static kernel buffers, then call `spawnProgram` with those copies. Provide a small fixed-capacity copy (e.g. `char abuf[16][128]; const char* ap[17];`). Document the cap (≤16 args, ≤127 chars each) and return `-E2BIG` (`-7`) if exceeded.

- [ ] **Step 4: Build + check-arch + test** — `make build`, `make check-arch`, `make test`.

- [ ] **Step 5: Prepare commit** — `feat: SYS_spawn syscall + dispatch (argv deep-copy, kernel-dir staging)`.

---

## Phase 4 — `SYS_stat` + ext metadata plumbing

Goal: `stat(path, struct stat*)` works (today only `fstat` by fd), and `ls -l` shows real mode/nlink/uid/gid/size/mtime read from the ext inode.

### Task 4.1: Extend FileStat + ExtFilesystem

**Files:** Modify `fs/Vfs.h`, `fs/ExtFilesystem.cpp`; Test `tests/test_statmap.cpp` (or extend an existing ext test).

- [ ] **Step 1: Read `fs/ExtFilesystem.cpp`** to find the inode struct field names (`i_mode`, `i_links_count`, `i_uid`, `i_gid`, `i_size`, `i_mtime`) and the `stat()` implementation that fills `FileStat`. (Investigate step — confirm exact names before editing.)

- [ ] **Step 2: Extend `FileStat`** in `fs/Vfs.h`:

```cpp
struct FileStat {
    NodeType type;
    unsigned size;
    unsigned mode;     // ext i_mode (type bits + perms)
    unsigned nlink;    // i_links_count
    unsigned uid;      // i_uid
    unsigned gid;      // i_gid
    unsigned mtime;    // i_mtime (epoch seconds)
};
```

- [ ] **Step 3: Fill them in `ExtFilesystem::stat`** from the inode (after the existing type/size assignment):

```cpp
out.mode  = inode.i_mode;
out.nlink = inode.i_links_count;
out.uid   = inode.i_uid;
out.gid   = inode.i_gid;
out.mtime = inode.i_mtime;
```

(Use the real field names found in Step 1.)

- [ ] **Step 4: Write a host test** in `tests/test_statmap.cpp` using the committed ext fixtures: stat a known file, assert `mode` has the regular-file bit (`0x8000`) and perms, `nlink>=1`, `size` matches the known fixture size. (Use the same fixture-loading pattern as the existing ext tests — read one to copy the setup.)

- [ ] **Step 5: Run** — `make test` (new assertions green; existing ext tests still green since fields are additive).

- [ ] **Step 6: Prepare commit** — `feat: surface ext inode metadata through FileStat`.

### Task 4.2: LinuxStat + SYS_stat + dispatch

**Files:** Modify `kernel/Syscall.h`/`.cpp`, `kernel/SyscallNr.h`, `kernel/SyscallDispatch.cpp`; Test extend `tests/test_syscall.cpp`.

- [ ] **Step 1: Number** — `kernel/SyscallNr.h`: `#define SYS_stat 106` (Linux i386 `stat`).

- [ ] **Step 2: Expand `LinuxStat`** in `kernel/Syscall.h` to the fields `ls` reads (kept simple, matching what the glue's `struct stat` maps):

```cpp
struct LinuxStat {
    unsigned st_mode;
    unsigned st_size;
    unsigned st_nlink;
    unsigned st_uid;
    unsigned st_gid;
    unsigned st_mtime;
    unsigned st_ino;
};
```

- [ ] **Step 3: Implement `Syscalls::stat`** in `Syscall.cpp`:

```cpp
int Syscalls::stat(String path, LinuxStat* out) {
    FileStat st;
    if (vfs->stat(path, st) < 0) return -ENOENT;
    out->st_mode  = st.mode ? st.mode
                  : ((st.type == NODE_DIR) ? 0x41ED : 0x81A4);  // fallback 0755/0644
    out->st_size  = st.size;
    out->st_nlink = st.nlink ? st.nlink : 1;
    out->st_uid   = st.uid;
    out->st_gid   = st.gid;
    out->st_mtime = st.mtime;
    out->st_ino   = 1;
    return 0;
}
```

Update `Syscalls::fstat` similarly to fill the new fields from `FileStat`. Declare `int stat(String, LinuxStat*);` in `Syscall.h`.

- [ ] **Step 4: Dispatch** — `kernel/SyscallDispatch.cpp`:

```cpp
case SYS_stat:
    ret = g_sys->stat(String((char*) a0), (LinuxStat*) a1);
    break;
```

- [ ] **Step 5: Test** — extend `tests/test_syscall.cpp`: open the ext fixture via the `Syscalls` core, `stat("/known", &ls)`, assert mode/size/nlink. `make test`.

- [ ] **Step 6: Build + check-arch + test** — all green.

- [ ] **Step 7: Prepare commit** — `feat: SYS_stat (by path) with real metadata`.

---

## Phase 5 — Port picolibc + glue (the userland libc)

Goal: a static `libc.a` for `i686-elf` (printf/stdio/qsort/malloc/string/time), built standalone against the existing cross-gcc, plus our thin glue (syscall stubs, `_sbrk`, pwd/grp, getdents). **This phase is build-integration and inherently iterative** — steps are command-driven with checkpoints.

> Note for the worker: do NOT rebuild the cross toolchain. picolibc builds as a separate library using the already-installed `i686-elf-gcc`. Keep all picolibc work in a **trailing Docker layer** so it never invalidates the toolchain cache.

### Task 5.1: Build picolibc in the Docker image

**Files:** Modify `docker/Dockerfile`; create `docker/picolibc-i686-elf.txt` (Meson cross file).

- [ ] **Step 1: Pin a picolibc version** — choose a release tag (e.g. `1.8.x`); record it. Add to the build either a vendored tarball under `user/third_party/picolibc/` or a `git clone --branch <tag> --depth 1` in the Dockerfile. Keep its `COPYING*`/license.

- [ ] **Step 2: Meson cross file** `docker/picolibc-i686-elf.txt`:

```ini
[binaries]
c = 'i686-elf-gcc'
ar = 'i686-elf-ar'
as = 'i686-elf-as'
strip = 'i686-elf-strip'
[host_machine]
system = 'none'
cpu_family = 'x86'
cpu = 'i686'
endian = 'little'
[properties]
c_args = ['-nostdlib', '-fno-leading-underscore', '-march=i686']
skip_sanity_check = true
```

(`-fno-leading-underscore` MUST match the kernel/user convention; confirm picolibc tolerates it, else drop it for the libc build and reconcile symbol names in the glue.)

- [ ] **Step 3: Add a trailing Dockerfile layer** (after lcov, so the toolchain cache is preserved): install meson+ninja+python3, then configure+build+install picolibc to `/opt/picolibc/i686-elf`:

```dockerfile
RUN pip3 install --break-system-packages meson ninja || apt-get install -y meson ninja-build
COPY picolibc-i686-elf.txt /tmp/
RUN git clone --depth 1 --branch 1.8.6 https://github.com/picolibc/picolibc /tmp/picolibc && \
    cd /tmp/picolibc && \
    meson setup build --cross-file /tmp/picolibc-i686-elf.txt \
       -Dprefix=/opt/picolibc/i686-elf -Dmultilib=false -Dpicocrt=false \
       -Dtests=false -Dformat-default=integer && \
    ninja -C build && ninja -C build install
```

- [ ] **Step 4: Rebuild the image** — `make docker-image` (expected: toolchain layers cached; only the new picolibc layer builds). Verify `libc.a` + headers exist: `$(DOCKER_RUN) ls /opt/picolibc/i686-elf/lib`.

- [ ] **Step 5: Prepare commit** — `build: port picolibc for i686-elf (trailing docker layer)`.

### Task 5.2: Syscall/sbrk glue + a minimal program that uses printf

**Files:** Create `user/libc-glue/syscalls.c`; modify `Makefile` (`USER_CFLAGS` to use picolibc includes + link `libc.a`), `arch/x86/cpu/usermode_x86.cpp` (map a heap window), `arch/include/arch/usermode.h`.

- [ ] **Step 1: Map a user heap** in `execUserImage` (after the image map, before the stack), e.g. `[imgEnd, USER_STACK_BOT)` as zeroed USER frames, and pass `heapStart`/`heapEnd` to the program via fixed symbols the glue reads. Simplest: fixed window `HEAP_BOT = (imgEnd)`, `HEAP_TOP = 0x4F0000`; the glue's `_sbrk` uses a static pointer initialized to a known base. Define `#define NX_HEAP_BASE 0x480000` and map `[0x480000, 0x4F0000)` (448 KiB heap) unconditionally; `_sbrk` bumps from `0x480000`. (Document: image must stay below `0x480000`; cat/ls/nsh are tiny.)

- [ ] **Step 2: Write `user/libc-glue/syscalls.c`** — the newlib stub contract over `int 0x80`:

```c
#include "SyscallNr.h"
#include <errno.h>
#include <sys/stat.h>

static inline int sys3(int nr,int a,int b,int c){int r;
  __asm__ __volatile__("int $0x80":"=a"(r):"a"(nr),"b"(a),"c"(b),"d"(c):"memory");return r;}

static int reterr(int r){ if(r<0){ errno=-r; return -1;} return r; }

int _write(int fd,const void*b,int n){ return reterr(sys3(SYS_write,fd,(int)b,n)); }
int _read (int fd,void*b,int n){ return reterr(sys3(SYS_read,fd,(int)b,n)); }
int _open (const char*p,int fl,int m){ (void)m; return reterr(sys3(SYS_open,(int)p,fl,0)); }
int _close(int fd){ return reterr(sys3(SYS_close,fd,0,0)); }
int _lseek(int fd,int off,int wh){ return reterr(sys3(SYS_lseek,fd,off,wh)); }
void _exit(int c){ sys3(SYS_exit,c,0,0); for(;;){} }
int _isatty(int fd){ return fd==0||fd==1||fd==2; }
int _getpid(void){ return 1; }
int _kill(int p,int s){ (void)p;(void)s; errno=EINVAL; return -1; }

extern char __heap_base;   /* not used; we hard-code the window */
void* _sbrk(int incr){
  static char* cur = (char*)0x480000;          /* NX_HEAP_BASE */
  char* top = (char*)0x4F0000;                  /* NX_HEAP_TOP  */
  if(cur+incr > top){ errno=ENOMEM; return (void*)-1; }
  char* prev=cur; cur+=incr; return prev;
}

int spawn(const char*path,char*const argv[]){ return sys3(SYS_spawn,(int)path,(int)argv,0); }
```

`_fstat`/`_stat` go in the next task (they need `struct stat` mapping). For now add minimal `_fstat` returning a char device for fd 0/1/2 so printf works:

```c
int _fstat(int fd,struct stat*st){ st->st_mode = S_IFCHR; return 0; }
```

- [ ] **Step 3: Switch userland build to picolibc** — `Makefile` `_userland`/`USER_CFLAGS`:
  - includes: `-isystem /opt/picolibc/i686-elf/include`
  - link: `-L/opt/picolibc/i686-elf/lib -lc` (and `-lgcc`), keep `-T user/nx.ld -nostdlib`.
  - Remove the old `user/libnanos.c` from the link; replace with `user/libc-glue/syscalls.c`.
  - Remove the temporary `__libc_init_array` stub (picolibc provides it).

- [ ] **Step 4: Replace `init.c` with a printf smoke program** (temporary) or adapt it: `printf("hello from picolibc %d\n", 42);`. `make build`. Iterate on link errors (missing stubs → add them; symbol-name/underscore mismatches → reconcile in the cross file or glue). This is the iterative checkpoint.

- [ ] **Step 5: QEMU headless** — boot, confirm `hello from picolibc 42` prints (proves stdio→`_write`→`int 0x80`→console and `_sbrk`/malloc inside printf work). No faults.

- [ ] **Step 6: Prepare commit** — `feat: picolibc syscall glue + user heap; programs use printf`.

### Task 5.3: `_fstat`/`_stat` mapping + pwd/grp + getdents backend

**Files:** Create `user/libc-glue/statmap.c`, `user/libc-glue/pwd_grp.c`, `user/libc-glue/dirent_backend.c` (+ headers if picolibc lacks `<pwd.h>`/`<grp.h>`).

- [ ] **Step 1: `_stat`/`_fstat`** — map the kernel `LinuxStat` (Phase 4 layout) into newlib's `struct stat`:

```c
#include "SyscallNr.h"
#include <sys/stat.h>
#include <errno.h>
struct knl_stat { unsigned mode,size,nlink,uid,gid,mtime,ino; };
static inline int sys3(int nr,int a,int b,int c){int r;
  __asm__ __volatile__("int $0x80":"=a"(r):"a"(nr),"b"(a),"c"(b),"d"(c):"memory");return r;}
static void fill(struct stat*o,const struct knl_stat*k){
  o->st_mode=k->mode; o->st_size=k->size; o->st_nlink=k->nlink;
  o->st_uid=k->uid; o->st_gid=k->gid; o->st_mtime=k->mtime; o->st_ino=k->ino;
}
int _stat(const char*p,struct stat*o){ struct knl_stat k;
  int r=sys3(SYS_stat,(int)p,(int)&k,0); if(r<0){errno=-r;return -1;} fill(o,&k); return 0; }
int _lstat(const char*p,struct stat*o){ return _stat(p,o); }   /* no symlinks */
```

(Confirm newlib's `struct stat` field set; some builds use `st_mtim.tv_sec` — adapt `fill` to whichever picolibc ships. Investigate the installed `<sys/stat.h>` first.)

- [ ] **Step 2: pwd/grp stubs** `user/libc-glue/pwd_grp.c`:

```c
#include <pwd.h>
#include <grp.h>
static struct passwd pw; static struct group gr;
struct passwd* getpwuid(uid_t uid){ pw.pw_name=(char*)"root"; pw.pw_uid=uid; return &pw; }
struct group*  getgrgid(gid_t gid){ gr.gr_name=(char*)"wheel"; gr.gr_gid=gid; return &gr; }
```

(If picolibc lacks `<pwd.h>`/`<grp.h>`, add minimal headers under `user/libc-glue/include/`.)

- [ ] **Step 3: getdents backend** — provide whatever symbol picolibc's `readdir` calls (often `getdents`/`_getdents`). Investigate the installed `<dirent.h>`/lib; implement over `SYS_getdents64`:

```c
#include "SyscallNr.h"
static inline int sys3(int nr,int a,int b,int c){int r;
  __asm__ __volatile__("int $0x80":"=a"(r):"a"(nr),"b"(a),"c"(b),"d"(c):"memory");return r;}
int getdents(int fd,void*buf,int n){ return sys3(SYS_getdents64,fd,(int)buf,n); }
```

(If picolibc has no `opendir`/`readdir` at all for `none` targets, implement a tiny `opendir/readdir/closedir` in this file over `open`+`getdents64`, matching `<dirent.h>`. Decide based on what the installed picolibc provides — investigate.)

- [ ] **Step 4: Build a `readdir` smoke** — temporary program listing `/boot` via `opendir/readdir`+`printf`. `make build`, QEMU, confirm names print. Iterate.

- [ ] **Step 5: Prepare commit** — `feat: stat/pwd/grp/getdents glue for picolibc`.

---

## Phase 6 — Vendor sbase + build `cat.nxe`

Goal: upstream `cat.c` compiles verbatim against picolibc+glue and runs.

### Task 6.1: Vendor sbase support files

**Files:** Create `user/third_party/sbase/{LICENSE,arg.h,util.h,compat.h,cat.c}` + needed `libutil/*`.

- [ ] **Step 1: Fetch verbatim** from the pinned sbase commit: `LICENSE`, `arg.h`, `util.h`, `compat.h`, `cat.c`, and the libutil objects `cat` links: `eprintf.c`, `concat.c`, `writeall.c`, `fshut.c`, `estrtonum.c` (+ `strtonum.c`) as referenced by `util.h`. Keep headers/copyright intact. Record the sbase commit hash in a `user/third_party/sbase/VERSION` file.

- [ ] **Step 2: Provide `argv0`** — sbase's `arg.h` declares `extern char *argv0;` and `ARGBEGIN` sets it. Ensure one TU defines `char *argv0;` (sbase programs define it in their own file via `ARGBEGIN`? No — it's defined in libutil's `eprintf.c` or each main). Confirm: sbase defines `char *argv0;` in `eprintf.c`. Vendoring `eprintf.c` provides it. ✓

- [ ] **Step 3: Build `cat.nxe`** — add a program entry to the generalized `_userland` (Task 7-build covers generalization; here add cat). Compile `cat.c` + the libutil files + glue + crt0 + nxhdr, link against picolibc, objcopy → `cat.nxe`. Iterate on missing libutil deps (add the referenced `.c` files). `USER_CFLAGS` adds `-Iuser/third_party/sbase`.

- [ ] **Step 4: Write to image** — `_image` writes `bin/cat.nxe` → `/bin/cat.nxe`.

- [ ] **Step 5: QEMU verify** — temporarily boot-exec `/bin/cat.nxe` with argv `["cat","/boot/grub/grub.cfg"]` (via a kernel test hook or once `nsh` exists). Confirm grub.cfg prints, exit 0. (Full interactive verify in Phase 8.)

- [ ] **Step 6: Prepare commit** — `feat: vendor sbase + build cat.nxe (verbatim)`.

### Task 6.2: Generalize the userland build to N programs

**Files:** Modify `Makefile` (`_userland`, `_image`), `arch/x86/arch.mk` if needed.

- [ ] **Step 1: Refactor `_userland`** into a pattern that builds a list `USER_PROGS = nsh cat ls` (nsh/ls added in later phases). Each prog links its own objects + the shared glue/crt0/nxhdr + picolibc, producing `bin/<prog>.nxe`. Use a make function or explicit rules. Keep `crt0.o`/`nxhdr.o`/glue objects shared.

- [ ] **Step 2: `_image`** loops the list writing each `bin/<prog>.nxe` → `/bin/<prog>.nxe` (replace the single `init.nxe` line; keep a `mkdir /bin`).

- [ ] **Step 3: Exclude third-party from coverage/check-arch** — they're userland (not in `MI_CHECK_DIRS`) and not in `TEST_MODULES`; verify `make check-arch` and `make test` ignore `user/`.

- [ ] **Step 4: Build** — `make build` produces all current `.nxe`. Prepare commit — `build: generalize userland to N programs`.

---

## Phase 7 — Build `ls.nxe` (verbatim, full libutf)

Goal: upstream `ls.c` compiles verbatim with libutf and runs `ls -l`.

### Task 7.1: Vendor libutf + ls.c

**Files:** Create `user/third_party/sbase/{ls.c,utf.h}` + `user/third_party/sbase/libutf/*` + extra libutil (`ereallocarray.c`, `reallocarray.c`, `estrdup.c`, `humansize.c`, `recurse.c` if pulled).

- [ ] **Step 1: Fetch verbatim** `ls.c`, `utf.h`, and sbase's `libutf` sources (`rune.c` + the runetype tables + `isprintrune`) at the pinned commit; plus the libutil files `ls` references (from the spec: `ereallocarray reallocarray estrdup humansize` + already-vendored `eprintf fshut`). Keep licenses.

- [ ] **Step 2: Reconcile headers** — `ls.c` includes `<sys/sysmacros.h>`, `<grp.h>`, `<pwd.h>`, `<time.h>`. Provide any missing ones under `user/libc-glue/include/` (e.g. a minimal `<sys/sysmacros.h>` with `major`/`minor`/`makedev`). Investigate which picolibc ships.

- [ ] **Step 3: time stub** — `ls -l` calls `localtime`/`strftime` (picolibc has them) but needs `time()`/`_gettimeofday`: add to glue a `_gettimeofday` returning a fixed epoch (e.g. the build date constant). So timestamps render consistently.

- [ ] **Step 4: Build `ls.nxe`** — add `ls` to `USER_PROGS` with its object set (ls.c + libutf + extra libutil). `make build`. Iterate on link errors (missing libutil/libutf objects, missing headers). This is the heaviest integration checkpoint.

- [ ] **Step 5: QEMU verify** — exec `/bin/ls.nxe` with argv `["ls","-l","/boot"]`; confirm a long listing with real mode bits + sizes (timestamps fixed). Also plain `["ls","/"]`. No faults.

- [ ] **Step 6: Prepare commit** — `feat: vendor sbase libutf + build ls.nxe (verbatim, ls -l)`.

---

## Phase 8 — `nsh` shell + boot target + cleanup

Goal: boot into `nsh$`; `ls`, `cat`, `echo`, `exit` work end-to-end.

### Task 8.1: Write `nsh`

**Files:** Create `user/nsh.c`; add `nsh` to `USER_PROGS`.

- [ ] **Step 1: Write `user/nsh.c`** (ours; picolibc available):

```c
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

int spawn(const char* path, char* const argv[]);   /* glue */

int main(void) {
    char line[256];
    for (;;) {
        write(1, "nsh$ ", 5);
        int n = read(0, line, sizeof line - 1);     /* blocking cooked line */
        if (n <= 0) { write(1, "\n", 1); return 0; } /* EOF (Ctrl-D) -> exit */
        if (line[n-1] == '\n') n--;
        line[n] = 0;
        if (n == 0) continue;

        char* argv[16]; int argc = 0;
        char* p = line;
        while (*p && argc < 15) {
            while (*p == ' ') *p++ = 0;
            if (!*p) break;
            argv[argc++] = p;
            while (*p && *p != ' ') p++;
        }
        argv[argc] = 0;
        if (argc == 0) continue;

        if (!strcmp(argv[0], "exit")) return argc > 1 ? atoi(argv[1]) : 0;
        if (!strcmp(argv[0], "echo")) {
            for (int i = 1; i < argc; i++) {
                write(1, argv[i], strlen(argv[i]));
                write(1, i + 1 < argc ? " " : "\n", 1);
            }
            if (argc == 1) write(1, "\n", 1);
            continue;
        }
        /* bare "ls" -> "ls /" (no cwd; keep sbase ls verbatim) */
        char* defargv[3];
        if (!strcmp(argv[0], "ls") && argc == 1) {
            defargv[0] = "ls"; defargv[1] = "/"; defargv[2] = 0;
            argv[0] = defargv[0]; argv[1] = defargv[1]; argv[2] = 0; argc = 2;
        }
        char path[160];
        snprintf(path, sizeof path, "/bin/%s.nxe", argv[0]);
        int rc = spawn(path, argv);
        if (rc == -2)   /* -ENOENT */
            printf("nsh: %s: command not found\n", argv[0]);
    }
}
```

- [ ] **Step 2: Build** — `make build` produces `nsh.nxe`. `_image` writes it to `/bin/nsh.nxe`.

- [ ] **Step 3: Prepare commit** — `feat: nsh interactive shell`.

### Task 8.2: Make `nsh` the boot target; retire the init demo

**Files:** Modify `kernel/Kernel.cpp`, `user/init.c` (or drop from `USER_PROGS`).

- [ ] **Step 1: Replace the `init ×2` loop** in `Kernel::start` with launching the shell:

```cpp
Console::writeLine("--- starting /bin/nsh.nxe ---");
const char* argv[] = { "/bin/nsh.nxe", 0 };
arch::execUserImage_byPath(vfs, "/bin/nsh.nxe", argv, 1);  // or reuse execProgram
```

Simplest: keep `execProgram(vfs, "/bin/nsh.nxe")` (it builds a 1-arg argv internally). The shell loops until `exit`. After it returns, drop to the idle loop.

- [ ] **Step 2: Clean up `init.c`** — remove the leftover `write(1,"test\n",5)` debug line; either keep `init` as a buildable demo prog or remove it from `USER_PROGS`. Decision: drop `init` from the boot path; keep the file but not built (or delete). Remove the temporary `__libc_init_array` stub if still present.

- [ ] **Step 3: Full QEMU demo** — `make run` (interactive): boot lands on `nsh$`. Run:
  - `ls -l /boot` → long listing, real mode/size.
  - `ls` → lists `/`.
  - `cat /boot/grub/grub.cfg` → prints it.
  - `echo hello world` → `hello world`.
  - `nope` → `nsh: nope: command not found`.
  - `exit` → returns to kernel idle.
  Headless corroboration (`-d int`): `int 0x80` traps at `cpl=3` for both `nsh` and spawned children; nested spawn returns; no `v=08/0d/0e`.

- [ ] **Step 4: Update ROADMAP.md** — mark "shell (nsh) + coreutils (cat/ls)" done under the roadmap's "later" list; note picolibc port landed (libc foundation toward `.ndl`). Update the memory pointer if needed.

- [ ] **Step 5: Prepare commit** — `feat: boot into nsh; retire init demo; roadmap update`.

---

## Self-review notes (gaps & decisions surfaced)

- **`execProgram` vs `spawnProgram`:** both stage at VA `0x400000`. `execProgram` (boot) runs under the kernel CR3 already (no user space active), so no pre-switch needed. `spawnProgram` runs under the parent's CR3, so it MUST `loadCr3(kernelDir)` before staging (Phase 3 Task 3.3 Step 2). Covered.
- **argv across CR3:** deep-copied into kernel buffers before the CR3 switch (Task 3.3 Step 3). Covered.
- **Heap below 0x480000:** image (`loadBase 0x400000`) + the `0x480000` heap base give 512 KiB for code/data — ample for cat/ls/nsh. If `ls.nxe` ever exceeds it, lower the heap base; documented in Task 5.2 Step 1.
- **picolibc symbol convention** (`-fno-leading-underscore`) is the top integration risk; Task 5.1 Step 2 + 5.2 Step 4 are the explicit reconcile points.
- **Coverage gate:** new host-tested MI units (LineDiscipline, UserStack, stat mapping) are added to `TEST_MODULES`/`COV_PATTERNS`; arch glue + third-party excluded (like `AtaBlockDevice`). Keeps the ≥90% gate honest.
- **`SYS_stat = 106`, `SYS_spawn = 500`** fixed in `SyscallNr.h`; no collision with existing numbers (1/3/4/5/6/19/108/220).
- **Spec coverage:** Phase 1↔Layer-1a, Phase 2↔1c, Phase 3↔1b, Phase 4↔1d, Phase 5↔Layer-2, Phase 6↔Layer-4 cat, Phase 7↔Layer-3+Layer-4 ls, Phase 8↔nsh + boot. All spec sections mapped.
```

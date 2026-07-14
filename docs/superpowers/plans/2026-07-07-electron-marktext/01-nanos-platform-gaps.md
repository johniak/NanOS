# 01 - NanOS Platform Gaps For Electron-Class Programs

**Goal:** Make NanOS provide enough Linux/POSIX-compatible runtime surface for Node, Chromium, and
Electron to run honestly, without app-specific hacks.

**Architecture:** Start from observed requirements. Build a gap matrix from Node and Chromium
configuration probes, then implement missing primitives in small, independently tested PRs.

**Entry criteria:** plan 00 gate green (`manifest/electron-stack.lock` exists,
`docs/en/electron-platform.md` has an empty "Platform Gap Matrix" section).

## Decisions

These are closed (see EXECUTION_GUIDE "Decisions Are Closed"):

- **eventfd + epoll: implement in the kernel, level-triggered first.** libuv and Chromium's
  message pump work with level-triggered epoll; `EPOLLET` is a documented follow-up only if a
  probe fails without it.
- **timerfd: do NOT implement up front.** libuv and Chromium schedule timers via
  `epoll_wait` timeouts. Implement only when a build/runtime probe fails, per the
  no-speculative-APIs rule.
- **sigaltstack: keep the existing "disabled" stub.** V8's stack-overflow checks are
  limit-based, not signal-based. Build V8 with the wasm trap handler off
  (`v8_enable_webassembly` trap-handler flags in plans 02/03) so SIGSEGV-on-altstack is never
  required. Revisit only if crash reporting needs it.
- **Shared memory: reuse the existing `memfd_create` emulation** (file under RamFs `/tmp`, see
  `user/libc-glue/posixstubs.c`). Add `F_ADD_SEALS` only if Chromium's shared-memory layer
  rejects unsealed memfds at runtime; record the probe result either way.
- **inotify: not implemented.** File watching is polling-based at the platform level (libuv
  `UV_FS_EVENT` fallback / `CHOKIDAR_USEPOLLING`). Plan 06 must not patch this per-app.
- **W^X policy: RW → RX flip via `mprotect`.** V8 writes code into RW pages then flips to RX.
  No permanent RWX mapping; any temporary bring-up exception needs a comment + follow-up task.

## Files

Likely modified areas:

- `kernel/SyscallNr.h`
- `kernel/SyscallDispatch.cpp`
- `kernel/Syscall.cpp`
- `kernel/Exec.cpp`
- `kernel/Futex.cpp`
- `fs/SynthFs.cpp`
- `user/libc-glue/*` and `user/libc-glue/include/*`
- `docs/en/electron-platform.md`
- `tests/test_*.cpp` (host tests)
- `user/eventfdtest.c`, `user/epolltest.c`, `user/mmapexectest.c`, `user/procselftest.c`,
  `user/shmdualtest.c` (QEMU microtests — registration recipe in EXECUTION_GUIDE)

Do not implement speculative APIs. Each addition must be tied to a failing Node/Chromium/Electron
probe or smoke.

## Task 1.1: Build The Gap Matrix

- [ ] Fill the `Platform Gap Matrix` section of `docs/en/electron-platform.md` as a table:
  `feature | status | evidence | consumer`. Statuses: `implemented`,
  `implemented-but-insufficient`, `missing-required`, `disabled-by-flag`, `not-needed-yet`.
- [ ] Seed it with these rows, **already verified against the tree on 2026-07-07** (re-verify with
  the given grep before trusting):

| Feature | Status (verified) | Evidence |
|---|---|---|
| `epoll_create1/ctl/wait` | missing-required | `grep -rli epoll kernel user/libc-glue` → empty |
| `eventfd` | missing-required | same grep, empty |
| `timerfd` | not-needed-yet (see Decisions) | same grep, empty |
| `memfd_create` | implemented (file-backed, no seals) | `user/libc-glue/posixstubs.c` ~line 397 |
| `sigaltstack` | stub reports disabled (see Decisions) | `user/libc-glue/posixstubs.c` ~line 203 |
| `dlopen` | permanently NULL — by design (`.ndl` model) | `user/libc-glue/posixstubs.c` ~line 196 |
| `inotify` | header only, no impl | `user/libc-glue/include/sys/inotify.h` |
| pthreads/TLS/futex | implemented (musl port) | memory: full pthread port, real munmap |
| `fork`/`execve`/`wait` | implemented | existing multiprocessing stack |
| `getrandom`, `/dev/urandom` | implemented (CSPRNG) | TLS/SSL stack depends on it |
| `/proc/self`, `/proc/<pid>/stat` (52 fields), `/proc/<pid>/task`, `cpuinfo`, `meminfo` | implemented (htop-level) | htop port |
| `clock_gettime` MONOTONIC/REALTIME | implemented | networking stack fix |

- [ ] Complete the matrix by probing the remaining rows (each row needs an evidence command):
  `pipe2`, `dup3`, `accept4`, `mmap`/`mprotect` exec pages, `madvise`, `mincore`,
  `MAP_FIXED_NOREPLACE`, `statx`/`statfs`/`fstatat`, `readlink("/proc/self/exe")`,
  `/proc/<pid>/fd`, signal masks in threads, DNS/socket options Chromium uses, sandbox syscalls.
  Probe method: `grep -rn "<symbol>" kernel/SyscallNr.h user/libc-glue/` plus, where unclear, a
  5-line user test.

## Task 1.2: Event Loop Primitives

Electron/Chromium will not be healthy on busy-poll `select` loops.

- [ ] **Step 1: Write the failing QEMU microtests first.** Create `user/eventfdtest.c`:

```c
/* eventfdtest — counter semantics, nonblocking, poll readiness, fork/dup. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <poll.h>
#include <sys/wait.h>
#include <sys/eventfd.h>

static int fails;
#define CHECK(cond, name) do { \
    if (cond) printf("  OK  : %s\n", name); \
    else { printf("  FAIL: %s\n", name); fails++; } } while (0)

int main(void) {
    printf("=== eventfdtest ===\n");
    int fd = eventfd(0, EFD_NONBLOCK);
    CHECK(fd >= 0, "eventfd(0, EFD_NONBLOCK) creates");

    uint64_t v = 0;
    CHECK(read(fd, &v, 8) == -1 && errno == EAGAIN, "read on zero counter -> EAGAIN");

    v = 3; CHECK(write(fd, &v, 8) == 8, "write 3");
    v = 4; CHECK(write(fd, &v, 8) == 8, "write 4");

    struct pollfd p = { .fd = fd, .events = POLLIN };
    CHECK(poll(&p, 1, 0) == 1 && (p.revents & POLLIN), "poll sees POLLIN");

    v = 0;
    CHECK(read(fd, &v, 8) == 8 && v == 7, "read drains accumulated 7");
    CHECK(read(fd, &v, 8) == -1 && errno == EAGAIN, "second read -> EAGAIN again");

    int fd2 = dup(fd);
    v = 1; CHECK(write(fd2, &v, 8) == 8, "write via dup");
    v = 0; CHECK(read(fd, &v, 8) == 8 && v == 1, "read via original sees dup write");

    pid_t pid = fork();
    if (pid == 0) { uint64_t c = 5; write(fd, &c, 8); _exit(0); }
    int st; waitpid(pid, &st, 0);
    v = 0;
    CHECK(read(fd, &v, 8) == 8 && v == 5, "child write visible after fork");

    printf(fails ? "eventfdtest: FAIL (%d)\n" : "eventfdtest: PASS\n", fails);
    return fails ? 1 : 0;
}
```

- [ ] **Step 2: Create `user/epolltest.c`:**

```c
/* epolltest — level-triggered readiness over pipe + eventfd, timeout, EPOLL_CTL_DEL,
 * close-while-waiting. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>

static int fails;
#define CHECK(cond, name) do { \
    if (cond) printf("  OK  : %s\n", name); \
    else { printf("  FAIL: %s\n", name); fails++; } } while (0)

static void *late_writer(void *arg) {
    int fd = *(int *) arg;
    usleep(200 * 1000);
    uint64_t v = 1; write(fd, &v, 8);
    return 0;
}

int main(void) {
    printf("=== epolltest ===\n");
    int ep = epoll_create1(0);
    CHECK(ep >= 0, "epoll_create1");

    int pfd[2]; pipe(pfd);
    int efd = eventfd(0, 0);
    struct epoll_event ev = { .events = EPOLLIN };
    ev.data.fd = pfd[0]; CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, pfd[0], &ev) == 0, "ADD pipe read end");
    ev.data.fd = efd;    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev) == 0, "ADD eventfd");

    struct epoll_event out[4];
    CHECK(epoll_wait(ep, out, 4, 0) == 0, "wait timeout=0 with nothing ready -> 0");

    write(pfd[1], "x", 1);
    int n = epoll_wait(ep, out, 4, 1000);
    CHECK(n == 1 && out[0].data.fd == pfd[0], "pipe write wakes with pipe fd");
    n = epoll_wait(ep, out, 4, 0);
    CHECK(n == 1, "level-triggered: still ready until drained");
    char c; read(pfd[0], &c, 1);
    CHECK(epoll_wait(ep, out, 4, 0) == 0, "drained -> not ready");

    pthread_t t; pthread_create(&t, 0, late_writer, &efd);
    n = epoll_wait(ep, out, 4, 2000);           /* blocks ~200ms, then wakes */
    CHECK(n == 1 && out[0].data.fd == efd, "blocking wait woken by other-thread eventfd write");
    pthread_join(t, 0);
    uint64_t v; read(efd, &v, 8);

    CHECK(epoll_ctl(ep, EPOLL_CTL_DEL, pfd[0], 0) == 0, "DEL pipe");
    write(pfd[1], "y", 1);
    CHECK(epoll_wait(ep, out, 4, 0) == 0, "deleted fd no longer reported");

    printf(fails ? "epolltest: FAIL (%d)\n" : "epolltest: PASS\n", fails);
    return fails ? 1 : 0;
}
```

- [ ] **Step 3: Register both tests** (EXECUTION_GUIDE recipe: mirror `mmaptest`'s three Makefile
  sites), build `make image64`, run in QEMU (`make run64`, log in, run `eventfdtest`) and confirm
  they FAIL today (link error or ENOSYS — either is the expected red state).
- [ ] **Step 4: Implement `eventfd`** in the kernel over the existing fd wait-queue substrate;
  add libc-glue wrapper + `sys/eventfd.h`. Host tests in `tests/` for counter bookkeeping.
- [ ] **Step 5: Implement `epoll_create1`/`epoll_ctl`/`epoll_wait`** over the same readiness
  substrate `poll` uses. If the current `poll` is timer-rescan, convert it to real wait-queue
  wakeups first — Chromium/libuv block in `epoll_wait` for seconds at a time.
- [ ] **Step 6: Run the gate.**

Gate:

```sh
make test64          # host tests green
make image64         # then in QEMU (run64, login jan/jan):
eventfdtest          # -> "eventfdtest: PASS"
epolltest            # -> "epolltest: PASS"
```

## Task 1.3: Memory Mapping And V8 Requirements

V8 requires precise memory behavior.

- [ ] **Step 1: Write `user/mmapexectest.c`** (expected to expose today's real `mprotect` status):

```c
/* mmapexectest — the V8 W^X contract: map RW, write code, flip to RX, execute, and verify
 * a write to the now-RX page faults (child dies by signal, parent survives). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

static int fails;
#define CHECK(cond, name) do { \
    if (cond) printf("  OK  : %s\n", name); \
    else { printf("  FAIL: %s\n", name); fails++; } } while (0)

int main(void) {
    printf("=== mmapexectest ===\n");
    size_t sz = 4096;
    unsigned char *p = mmap(0, sz, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED, "anonymous RW map");

    /* x86_64: mov eax, 42; ret */
    static const unsigned char code[] = { 0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3 };
    memcpy(p, code, sizeof code);

    CHECK(mprotect(p, sz, PROT_READ | PROT_EXEC) == 0, "mprotect RW -> RX");
    int (*fn)(void) = (int (*)(void)) p;
    CHECK(fn() == 42, "execute flipped page returns 42");

    pid_t pid = fork();
    if (pid == 0) { p[0] = 0x90; _exit(0); }   /* write to RX page: must die */
    int st; waitpid(pid, &st, 0);
    CHECK(WIFSIGNALED(st), "write to RX page faults the child (W^X holds)");

    CHECK(mprotect(p, sz, PROT_READ | PROT_WRITE) == 0, "flip back RX -> RW");
    p[0] = 0x90;
    CHECK(1, "write after flip-back succeeds");
    munmap(p, sz);

    printf(fails ? "mmapexectest: FAIL (%d)\n" : "mmapexectest: PASS\n", fails);
    return fails ? 1 : 0;
}
```

- [ ] **Step 2: Register + run it.** Whatever fails is the work list: complete `mprotect` for
  user pages (all three transitions above) in the kernel.
- [ ] **Step 3:** Add `MAP_FIXED_NOREPLACE` and `madvise` accepted-no-op cases **only when** the
  Node/V8 build (plan 02) actually probes them; add the matrix row first.
- [ ] Gate: `mmapexectest` prints PASS in QEMU; `make test64` green.

## Task 1.4: Process, Procfs, And Path Introspection

- [ ] **Step 1: Write `user/procselftest.c`:**

```c
/* procselftest — /proc/self/exe readlink, /proc/self/fd listing, cwd round-trip. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>

static int fails;
#define CHECK(cond, name) do { \
    if (cond) printf("  OK  : %s\n", name); \
    else { printf("  FAIL: %s\n", name); fails++; } } while (0)

int main(void) {
    printf("=== procselftest ===\n");
    char buf[256]; ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    CHECK(n > 0, "readlink /proc/self/exe");
    if (n > 0) { buf[n] = 0; CHECK(strstr(buf, "procselftest") != 0, "exe path names this binary"); }

    int fd = open("/tmp/.procself-probe", O_CREAT | O_RDWR, 0644);
    DIR *d = opendir("/proc/self/fd");
    CHECK(d != 0, "opendir /proc/self/fd");
    int seen = 0;
    if (d) { struct dirent *e; char want[16];
        snprintf(want, sizeof want, "%d", fd);
        while ((e = readdir(d))) if (!strcmp(e->d_name, want)) seen = 1;
        closedir(d); }
    CHECK(seen, "open fd listed in /proc/self/fd");
    close(fd); unlink("/tmp/.procself-probe");

    char cwd[256];
    CHECK(getcwd(cwd, sizeof cwd) != 0, "getcwd");
    CHECK(chdir("/tmp") == 0 && getcwd(buf, sizeof buf) && !strcmp(buf, "/tmp"),
          "chdir/getcwd round-trip");

    printf(fails ? "procselftest: FAIL (%d)\n" : "procselftest: PASS\n", fails);
    return fails ? 1 : 0;
}
```

- [ ] **Step 2:** Implement whatever fails: `/proc/self` resolution, `/proc/self/exe` readlink to
  the executable path, `/proc/<pid>/fd` directory listing. The htop port already added most of
  `/proc/<pid>/stat`; extend `fs/SynthFs.cpp`, don't fork a new mechanism.
- [ ] **Step 3:** Expand `/proc/cpuinfo` and `/proc/meminfo` fields **only** when a Node/Chromium
  probe reads a missing field (matrix row first).
- [ ] Gate: `procselftest` prints PASS in QEMU; `make test64` green.

## Task 1.5: Shared Memory Across Processes

Chromium uses shared memory heavily. `memfd_create` already exists (file-backed under RamFs
`/tmp`); this task proves the sharing semantics rather than implementing from scratch.

- [ ] **Step 1: Write `user/shmdualtest.c`:**

```c
/* shmdualtest — two processes map the same memfd MAP_SHARED and observe each other's writes. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

static int fails;
#define CHECK(cond, name) do { \
    if (cond) printf("  OK  : %s\n", name); \
    else { printf("  FAIL: %s\n", name); fails++; } } while (0)

int main(void) {
    printf("=== shmdualtest ===\n");
    int fd = memfd_create("shmdual", 0);
    CHECK(fd >= 0, "memfd_create");
    CHECK(ftruncate(fd, 4096) == 0, "ftruncate 4096");

    volatile char *p = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(p != MAP_FAILED, "parent MAP_SHARED");

    pid_t pid = fork();
    if (pid == 0) {
        volatile char *c = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (c == MAP_FAILED) _exit(2);
        while (c[0] != 'A') usleep(10 * 1000);   /* wait for parent write */
        c[1] = 'B';
        _exit(0);
    }
    p[0] = 'A';
    int st; waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "child mapped and saw parent write");
    CHECK(p[1] == 'B', "parent sees child write");

    printf(fails ? "shmdualtest: FAIL (%d)\n" : "shmdualtest: PASS\n", fails);
    return fails ? 1 : 0;
}
```

- [ ] **Step 2:** Fix whatever fails (most likely candidate: true `MAP_SHARED` write-back through
  the RamFs file). Host tests for the bookkeeping in `tests/`.
- [ ] Gate: `shmdualtest` prints PASS in QEMU; `make test64` green.

## Task 1.6: File Watching Policy

Per Decisions: no inotify. This task is documentation plus one guard:

- [ ] Add matrix row: `inotify: disabled-by-policy — polling watchers`.
- [ ] Record in `docs/en/electron-platform.md` (Debugging section) that Electron apps on NanOS
  run with `CHOKIDAR_USEPOLLING=1` set by the launcher (plan 05 implements this).
- [ ] Add an idle-CPU assertion to the eventual `scripts/smoke-marktext.sh` contract (plan 06):
  idle MarkText under polling must stay below the agreed CPU threshold.

## Task 1.7: Fonts, Locales, ICU, Certificates

- [ ] Define in `docs/en/electron-platform.md` where Electron runtime data files live:
  - ICU: `/disks/main/nanos/lib/electron/icudtl.dat`;
  - locales: `/disks/main/nanos/lib/electron/locales/`;
  - fonts: the existing NanOS font directory (reuse the bundled IBM Plex Sans / JetBrains Mono;
    find the exact image path with `grep -rn "Plex" Makefile user/` and record it);
  - certificates: the bundle already used by the OpenSSL/TLS port (record its path the same way).
- [ ] Add image install rules for any data file not yet staged.
- [ ] Font discovery must be deterministic: Chromium gets a fixed directory (plan 03 subplan 04
  wires it via Skia); no host paths, no fontconfig.
- [ ] Gate: the paths are written in the doc and exist in a freshly built image
  (`make image64` then check via `run64` shell: `ls /nanos/lib/electron` may still be empty at
  this stage — the gate here is the documented decision plus staged fonts/certs).

## Task 1.8: Sandbox Decision

Electron/Chromium has a Linux sandbox model NanOS does not provide.

- [ ] Disable the Chromium sandbox through supported build/runtime flags only
  (`--no-sandbox` runtime default set by the NanOS launcher; GN-level `is_official_build=false`
  path — plan 03 subplan 00 owns the GN args).
- [ ] Document this in `docs/en/electron-platform.md` as a security limitation.
- [ ] The flag lives in runtime launch defaults (plan 05 launcher), never per-app.
- [ ] Create a follow-up note (one line in the doc) for a real NanOS sandbox later.

## Stage Demo

Demo app: **uv-echo** — the canonical libuv TCP echo-server example running on NanOS.

Why libuv: it is open source, it is the exact first consumer of the new primitives (epoll +
eventfd; timers via `epoll_wait` timeout, matching the Decisions), and plan 02 builds the same
pinned libuv inside Node anyway — nothing here is throwaway. It reuses only existing pieces
(SDK port flow, TCP/IP stack, nsh) plus this stage's new syscalls.

- [ ] Build libuv from the pinned Node checkout (`$SDK_WORK/node-src/deps/uv`) as a static lib in
  the SDK container; link its `tcp-echo-server` example plus a 1 s `uv_timer` heartbeat into a
  single `uvdemo.nxe` (`--server` / `--client` modes).
- [ ] Demo scenario: start the server, run the client from a second guest process, see
  `uv-echo: RTT OK` plus timer ticks on screen — the new event-loop syscalls visibly driving
  Node's own event loop library.
- [ ] Test: `scripts/smoke-uvdemo.sh` (house style) asserting the OK marker + timer marker,
  proven able to fail.
- [ ] Fallback (only if standalone libuv turns out to need real porting work, which would be
  duplicated in plan 02): an in-tree `user/epollechod.c` echo server (~150 lines) exercising
  epoll+eventfd the same way; record the reason in the status row.

## Gate

- [ ] Gap matrix complete: every row has status + evidence; no `TBD` cells.
- [ ] `eventfdtest`, `epolltest`, `mmapexectest`, `procselftest`, `shmdualtest` all print PASS in
  QEMU.
- [ ] Stage demo green (`scripts/smoke-uvdemo.sh` or the recorded fallback).
- [ ] `make test64` passes.
- [ ] `status.md`: `M1 Platform Gap Matrix` checked, log row added.

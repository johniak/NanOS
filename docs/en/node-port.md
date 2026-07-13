# Node.js on NanOS (x86_64)

The record of porting Node.js (and its embedded V8) to NanOS, for the Electron/MarkText program
(plan `docs/superpowers/plans/2026-07-07-electron-marktext/02-node-v8.md`). One V8/Node port serves
both a standalone `node.nxe` and Electron.

## Source

Pinned by `manifest/electron-stack.lock`: **node `v24.15.0`**. The Electron fork
(`https://github.com/electron/node`) versions on branches, not release tags, so `v24.15.0` is not a
fetchable tag there — per the plan's documented fallback, `scripts/electron/fetch-node.sh` clones
upstream **`nodejs/node` at `v24.15.0`** instead and records the exact commit in
`$SDK_WORK/node-src/NANOS_REVISION.txt`. The Electron build (plan 04) will re-pin to Electron's own
Node during the Chromium/Electron integration; standalone Node uses upstream at the same version.

Component versions (from the source tree):

| Component | Version |
|---|---|
| node | v24.15.0 |
| V8 | 13.6.233.17 |
| libuv | 1.51.0 |
| OpenSSL | bundled (Node's `deps/openssl`, `--openssl-no-asm`) |

## Build identity & toolchain

- Host: Apple Silicon (aarch64). The `x86_64-nanos` cross toolchain (GCC 14.2.0) is an **aarch64→
  x86_64-nanos** Canadian cross, so it runs **natively** in the `nanos-sdk-dev` arm64 container — no
  emulation, full-speed builds. Host build tools (Python 3.13, native g++ 14.2, make, ninja) are in
  the same image.
- `--dest-os=linux --dest-cpu=x64 --cross-compiling` (gyp has no pluggable OS list worth fighting;
  NanOS is close enough to linux/musl).
- **`-D__linux__` on the target build.** gyp already selects the linux source files, but the
  picolibc-based nanos gcc does not advertise `__linux__`, so the `#ifdef __linux__` blocks *inside*
  those files stayed dark and broke (e.g. abseil `poison.cc` references a `data` variable only
  declared under `ABSL_HAVE_MMAP`, which is gated on `__linux__`). Defining `__linux__` makes the
  compiler agree with gyp's dest-os. picolibc itself is OS-agnostic and does not key off `__linux__`.
  NanOS-specific divergence stays behind `-D__NANOS__`, which wins where the two differ.
- Native addons are **statically registered** (`NODE_MODULE_LINKED` + `--link-module`); there is no
  `.node` `dlopen` (NanOS has no ELF `.so` loader). A `.ndl`-backed `process.dlopen` is a follow-up.
- First build shape is small: `--without-intl --without-inspector --without-node-snapshot
  --without-npm --without-corepack`, V8 wasm trap handler off (no sigaltstack — plan 01).

## Compiler-flag wrappers

`ports/node/nx-node-cc` / `nx-node-cxx` wrap the target compilers and strip flags the nanos gcc
rejects, forwarding everything else. Each stripped flag is a deliberate porting decision:

| Flag | Why stripped |
|---|---|
| `-pthread` | gyp adds it for linux targets; NanOS threading lives inside libc (musl-pthread on picolibc), so there is no separate `-lpthread` and the driver flag is unknown to the nanos gcc. |

## libc-glue additions for Node

| API | Resolution | Consumer |
|---|---|---|
| `syscall()` | **Now a real generic 6-arg multiplexer** (`posixstubs.c`) — the kernel uses the Linux x86_64 numbers, so `syscall(n, …)` forwards straight to it (unimplemented → -ENOSYS). Subsumes the old SYS_kcmp-only stub. This is what lets V8/abseil raw-syscall paths (`syscall(SYS_write)`, futex, `sched_*`) work without per-site patches. | abseil `raw_logging`, V8 base |
| `<syscall.h>` → full number set | Pulls the kernel's authoritative `SyscallNr.h` (its `__x86_64__` section = Linux numbering) so `SYS_write`/`SYS_mmap`/… are defined at compile. | abseil, V8 |
| `mlock` / `munlock` / `mlockall` / `munlockall` | Success no-ops (`posixstubs.c`) — NanOS has no swap and never reclaims a mapping, so pages are always resident. | OpenSSL secure heap (`mem_sec.c`) |
| `prctl` | Forwards through `syscall()` (kernel has no prctl → -ENOSYS); callers use it only for advisory VMA naming and ignore the result. `<sys/prctl.h>` added. | abseil `low_level_alloc` |
| `<linux/futex.h>` | `FUTEX_*` op constants (kernel has a Linux-ABI `SYS_futex`). | abseil SpinLock |
| `<sys/ucontext.h>` / `<ucontext.h>` | glibc-compatible x86_64 `ucontext_t` (register file; `gregs[16]`=RIP) so crash backtracers compile. Runtime backtrace is best-effort. | abseil stacktrace |

## Compiler-define decisions (`build-node.sh`)

| Define / flag | Why |
|---|---|
| `-D__linux__` | Make the compiler agree with gyp's `--dest-os=linux` (see above). |
| `-D_GNU_SOURCE` | Raise picolibc's `__POSIX_VISIBLE` so the full POSIX/glibc surface is declared. |
| `-D_POSIX_SEM_VALUE_MAX=32767` | picolibc omits it; libstdc++ C++20 `<semaphore>` needs it in a `static_assert`. |
| `-g0` / `-j4` / `--memory=14g` | Build fits disk + the Docker memory limit (V8 debug objects are 12+ GiB; compiles are ~2 GiB each). |

## Source patches (`ports/node/patches/`)

| Patch | Why |
|---|---|
| `0001-abseil-direct-mmap-use-libc-mmap` | abseil's `DirectMmap` issues a raw Linux `mmap` syscall (addr as arg0); NanOS's `mmap`/`mmap2` ABI takes no addr, so route NanOS (`__NANOS__`) to the portable libc-`mmap()` branch. |
| `0002-abseil-wcslen-global` | The nanos libstdc++ `<cwchar>` hides `std::wcslen` (see below); use global `::wcslen`. |

## BLOCKER — libstdc++ has no threading model (toolchain rebuild needed)

The build reaches deep into V8 (openssl done; abseil compiling) and then hits a hard wall that is **not**
a per-file patch:

- The SDK's libstdc++ was configured `--enable-threads=single`: `_GLIBCXX_HAS_GTHREADS` is `#undef` in
  `x86_64-nanos/bits/c++config.h`, so **`std::mutex`, `std::condition_variable`, `std::thread`,
  `std::call_once`, `std::shared_mutex` do not exist**. abseil's `stdcpp_waiter.h` (and, later, Node
  and V8 themselves) require them.
- Independently, the base `libc.a` in the SDK sysroot contains **zero pthread symbols** — the NanOS
  pthread implementation (musl-pthread port) is built separately and is not part of the sysroot the
  cross toolchain links against.

**Resolution (next phase):** rebuild the SDK's libstdc++ with `--enable-threads=posix` on top of the
NanOS pthread port, and stage `pthread.h` + the pthread objects into the `x86_64-nanos` sysroot so
`node.nxe` can link them. GCC's `gthr-posix` maps directly onto the pthread API the port already
provides; `pthread.h` is already in the sysroot. This is a toolchain rebuild (the `build-gcc` tree is
present at `$SDK_WORK/build-gcc`), and it modifies the shared SDK toolchain, so it is called out as a
distinct step rather than folded into the source-level porting above.

## Status

`configure` OK · libuv host tools OK · **OpenSSL target OK** · **V8/abseil compiling** → blocked at
the libstdc++-threads wall above. ~13 source-level porting issues resolved (wrappers, libc-glue APIs,
headers, defines, 2 abseil patches). The remaining work is gated on the toolchain rebuild.

## Toolchain-shim fixes triggered by C++

- **`nx-dllimport.h` made C++-safe.** The data-import shim (`__imp_*` IAT slots for picolibc's
  `stdout`/`environ`/`_ctype_b`/...) declared its slots with default (C++) linkage. Under C++ (all of
  V8), the system headers expand `environ`/`_ctype_b` to those same slots *inside* their
  `__BEGIN_DECLS` `extern "C"` block, so the two collided ("conflicting declaration ... with 'C'
  linkage"). Every prior port was pure C, so this never surfaced. Fixed with an `NX_IMP` macro
  (`extern "C"` under C++, plain `extern` under C) — byte-identical for the existing C ports.

## Build resource notes

- `-j4` inside a 14 GiB Docker memory limit: V8 C++ compiles are RAM-heavy (~2 GiB each); `-j8` OOMs
  ("cannot allocate memory" from cc1plus).
- `-g0` (overrides gyp's `-g`): a debug V8 is 12+ GiB of objects. The first bring-up needs no symbols.
- The host baseline build (Task 2.1) and the cross build must NOT share `out/Release` (the same
  checkout, different `dest-os`) — run them sequentially / clean between.

## Status

Cross build in progress. Reached: `configure` OK; libuv (host tools) OK; **OpenSSL target OK**; into
**V8 (abseil / torque-generated)**. Remaining bring-up (V8 platform layer, link, `.nxe` conversion,
first `--jitless` then JIT execution in QEMU) tracked in the plan-02 status rows.

## Known-untested (to fill as gates land)

- `child_process` spawn (libuv process on NanOS), `worker_threads`, `https`/TLS end to end, `dns`.

# libc-glue & porting: missing headers / symbols / GNU-isms (troubleshooting)

This is the reference for the **most common port-build failures** on NanOS: a missing header
(`fatal error: stdio_ext.h: No such file`), a missing function at link time
(`undefined reference to '__fpending'`), or autoconf mis-detecting a libc feature. They almost
always trace back to **how the C library is assembled**, so read §1–§3 first, then the
symptom→fix table in §4.

> TL;DR for a fresh machine: a missing `<stdio_ext.h>` (and most other glue headers) means the
> **sysroot was never synced with the NanOS libc-glue** — run `sync-sysroot` (§3). The glue
> headers do NOT come baked into the SDK toolchain; they are injected from a *built* NanOS
> checkout.

---

## 1. The C library is three layers

NanOS has no single libc. A ported program is compiled and linked against three stacked pieces:

```
   the program (vim, openssl, bash, sbase coreutils, …)
        │  #include <stdio.h>, <stdio_ext.h>, <pthread.h>, <netdb.h>, …
        ▼
1) picolibc            the freestanding base C library (printf/malloc/string/math/<stdio.h>…).
                       Built for x86_64-elf by the nanos-sdk Dockerfile. Knows nothing about
                       NanOS or GNU/glibc extensions.
        +
2) libc-glue           the GNU/POSIX-on-NanOS shim that fills picolibc's gaps — the glibc-isms
   (user/libc-glue/)   real programs assume exist. HEADERS in user/libc-glue/include/*.h,
                       IMPLEMENTATIONS in user/libc-glue/*.c (+ pthread/*.c). Compiled into
                       libc.ndl; the headers are copied into the sysroot.
        +
3) kernel syscalls     the actual primitives. libc-glue's .c bottoms out in `int 0x80`/SYS_*
   (kernel/SyscallNr.h) numbers; libc.ndl imports the kernel export table by name.
```

So "`stdio_ext.h` is missing" is **never** a picolibc bug — that header is a GNU extension that
lives in **layer 2** (`user/libc-glue/include/stdio_ext.h`, impl `user/libc-glue/stdio_ext.c`)
and only reaches a port if layer 2 was merged into the sysroot.

---

## 2. Where everything lives

| Thing | Path | Notes |
|---|---|---|
| Glue **headers** (GNU/POSIX) | `user/libc-glue/include/*.h` | `stdio_ext.h`, `pthread.h`, `netdb.h`, `shadow.h`, `pty.h`, `dlfcn.h`, `mntent.h`, `utmp.h`, `syslog.h`, `crypt.h`, `poll.h`, `dirent.h`, `arpa/`, `net/`, `netinet/`, `sys/`, `linux/` … |
| Glue **implementations** | `user/libc-glue/*.c`, `user/libc-glue/pthread/*.c` | `stdio_ext.c`, `syscalls.c`, `posixstubs.c`, `resolv*.c`, `pwd_grp.c`, `grp_shadow.c`, `crypt.c`, `sockets.c`, `termios.c`, `dirent.c`, `getdelim.c`, `cwd.c`, `tls.c`, `ifname.c`, `ptyutil.c`, `retarget_lock.c` |
| Central forced decls | `user/libc-glue/compat-decls.h` | declarations picolibc omits but vendored sources reference (`lstat`, `renameat`, `mknod`, `clock_gettime`/`CLOCK_*`, …). `-include`'d into ports so sources stay unmodified. |
| dllimport shim | `user/libc-glue/nx-dllimport.h` | maps DATA exports (`errno`, `stdin/stdout/stderr`) to libc.ndl IAT slots. x86_64 needs it `-include`'d (RIP-relative `R_X86_64_PC32` can't be auto-imported); i686 skips it. |
| Syscall numbers | `kernel/SyscallNr.h` | copied into the sysroot; the contract between glue and kernel. |
| The link library | `bin/libc.ndl` + `bin/libc.ndl.a` | `libc.ndl` = runtime import-by-name module; `libc.ndl.a` = the static import lib a port links as `-lc`. Built from `libc.elf` (picolibc + all glue .o) via mknx. |
| picolibc base | nanos-sdk Dockerfile (`picolibc-x86_64-elf.txt`) | baked into the SDK image's sysroot at `/opt/x86_64-nanos`. |

The glue `.c` are compiled by the `bin/%.o: user/libc-glue/%.c` rule and linked into `libc.elf`
→ `objcopy` → `libc.ndl`. So **adding a function** = drop a `.c` in `user/libc-glue/`, declare it
in the matching header, rebuild `libc.ndl`.

---

## 3. How the sysroot gets the glue — `sync-sysroot` (the step people miss)

The SDK toolchain image (`nanos-sdk-dev`) ships **only picolibc** in its sysroot. The libc-glue
headers + the NanOS link bits are injected **at use time, from a built NanOS checkout**, by
`nanos-sdk/sysroot/sync-sysroot`:

```sh
# from the nanos-sdk repo; needs a BUILT NanOS (bin/libc.ndl.a, crt0.o, nxhdr.o, nx.ld present)
sync-sysroot --nanos=/path/to/NanOS \
             --prefix=$SDK_TC \
             --picolibc=/opt/picolibc/x86_64-elf
```

What it does (in order):
1. `cp picolibc/include` → sysroot `include/`  (the base headers)
2. `cp user/libc-glue/include/.` → sysroot `include/`  ← **this is where `stdio_ext.h` etc. arrive**
3. `cp kernel/SyscallNr.h` → sysroot
4. `posix-hosted-patch.sh` — adapts picolibc headers so `__nanos__` is a *hosted* POSIX target
   (declares `lstat`/`mknod`, `nanosleep`/`clock_gettime`, sigaction flags, `getrlimit`).
5. `cp bin/{libc.ndl.a→libc.a, crt0.o, nxhdr.o}` + `user/nx.ld` → sysroot `lib/`  (the link bits;
   re-runnable because `libc.ndl` evolves with the kernel).

Each per-port Makefile target ALSO does `cp -R user/libc-glue/include/. $(SDK_TC)/<triple>/include/`
as a redundant safety copy — but if you build a port *without* its prereqs, or run the toolchain
standalone, only `sync-sysroot` guarantees the glue is present.

**=> If a fresh sysroot is missing `<stdio_ext.h>` (or any glue header): you have not run
`sync-sysroot` against this NanOS checkout. Build NanOS first (`make ARCH=x86_64 bin/libc.ndl.a
bin/crt0.o bin/nxhdr.o`), then `sync-sysroot`.**

---

## 4. Symptom → cause → fix

| Symptom | Cause | Fix |
|---|---|---|
| `fatal error: stdio_ext.h: No such file` (or `pty.h`, `shadow.h`, `netdb.h`, …) | sysroot has picolibc but not libc-glue | run **`sync-sysroot`** (§3); or re-run the port target (it copies `libc-glue/include`) after building libc |
| `fatal error: <some GNU header>` that is NOT in `user/libc-glue/include/` | genuinely not provided yet | add the header to `user/libc-glue/include/` (see §5) |
| link: `undefined reference to '__fpending'` (or `getprogname`, `strchrnul`, …) | header declares it but no impl in libc.ndl | add/extend a `.c` in `user/libc-glue/`, rebuild `libc.ndl`, re-`sync-sysroot` |
| autoconf "checking for X… yes" then link fails on X | conftest linked against a *declaration* with no symbol → false positive | the **honest-conftest wrapper** (§6) must be active so autoconf's link test fails honestly |
| `undefined reference` to a syscall-backed fn (`clock_gettime`, `renameat`) | declared in `compat-decls.h` but not `-include`'d, or SYS_* missing | ensure the port `-include`s `compat-decls.h`; check `kernel/SyscallNr.h` has the number + a dispatch case |
| x86_64 only: `R_X86_64_PC32` / unresolved `errno`/`stdout` at mknx | `nx-dllimport.h` not force-included | port CFLAGS must `-include user/libc-glue/nx-dllimport.h` (the nxport hook does this) |
| `libc.a`/`crt0.o` not found by the cross gcc | link bits never synced | `sync-sysroot` step 5 (needs a built NanOS) |

---

## 5. Worked example — adding a missing GNU header/function

`<stdio_ext.h>`'s `__fpending` (how it was actually added; mirror this for the next gap):

1. **Header** — `user/libc-glue/include/stdio_ext.h`:
   ```c
   #ifndef _STDIO_EXT_H
   #define _STDIO_EXT_H
   #include <stdio.h>
   size_t __fpending(FILE *fp);
   /* … __freadahead/__fpurge etc. as ports need them … */
   #endif
   ```
2. **Implementation** — `user/libc-glue/stdio_ext.c` (bottoming out in picolibc internals or a
   syscall). Keep it minimal and honest — return real values, don't stub silently.
3. **Build** — the `bin/%.o: user/libc-glue/%.c` rule compiles it into `libc.elf` → `libc.ndl`.
   `make ARCH=x86_64 bin/libc.ndl bin/libc.ndl.a`.
4. **Publish to the sysroot** — `sync-sysroot` (or the port's `cp -R libc-glue/include`).
5. Rebuild the port.

Do NOT fake detection or silently stub a function a program relies on — see the SDK's
"honest autoconf" rule (§6) and the project memory `no-shortcuts-on-foundations`.

---

## 6. Honest autoconf / conftest (why detection must be truthful)

Many GNU ports `./configure`-probe for libc functions by **compiling and linking** a tiny
conftest. NanOS's cross gcc is wrapped by `nanos-sdk/toolchain/conftest-strict-wrapper.sh`
(installed as `<triple>-gcc`, real compiler at `<triple>-gcc.real`). It makes conftest **link**
tests fail when the symbol is only *declared* (header present) but not *defined* (no impl in
libc.ndl) — so autoconf doesn't record a false "yes" that explodes at the real link. If a port
mis-detects a function, check this wrapper is the active `gcc` in the sysroot `bin/`.

---

## 7. Fresh-machine bootstrap checklist (the order that avoids these errors)

1. **SDK image** — `cd nanos-sdk && docker build -t nanos-sdk-dev .` (builds the x86_64-nanos
   cross toolchain + picolibc; the long, one-time step).
2. **Toolchain → `SDK_TC`** — extract `/opt/x86_64-nanos` from the image into
   `~/Projects/nanos-sdk-work/toolchain` (the dir the NanOS Makefile mounts).
3. **Build NanOS libc** — `cd NanOS && make ARCH=x86_64 bin/libc.ndl bin/libc.ndl.a bin/crt0.o bin/nxhdr.o`.
4. **`sync-sysroot`** — inject libc-glue headers + link bits into `SDK_TC` (§3). *Skipping this is
   the #1 cause of "missing `<stdio_ext.h>`".*
5. **Library-prereq ports** — `make ARCH=x86_64 zlib ncurses libpng libjpeg`.
6. **App ports** — `make ARCH=x86_64 vim htop grep …`, `make bash git openssl dropbear netsurf …`.
7. **Image** — `make assets && make image64`.

See also: [`nxe-ndl.md`](nxe-ndl.md) (the .nxe/.ndl format + mknx), [`syscalls.md`](syscalls.md),
[`writing-apps.md`](writing-apps.md), and CLAUDE.md "Building the FULL system".

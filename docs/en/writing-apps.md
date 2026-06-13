# Writing applications for NanOS

A complete guide for anyone who wants to build a new application that runs on NanOS —
from "hello world" to porting a real Linux application (like `wget` or `ping`).

This document describes **two independent paths** and both machineries behind them:

1. **Built-in application (in-tree)** — code in `user/*.c`, built by the NanOS `Makefile`
   using the containerized picolibc. This is how `nsh`, `cat`, `ls`, `free`, the `nwm`
   compositor, Doom, etc. are produced.
2. **Porting an external application** — an existing program (GNU/BSD) built by a separate
   cross-toolchain **nanos-sdk** (`i686-nanos`) driven by an `nxport.toml` manifest. This is
   how `wget`, `ping`, `vim`, `grep`, `inetd`, `darkhttpd` are produced.

The common denominator of both paths is the **`.nxe`** executable format and the import-by-name
dynamic linking model (`.ndl`). So we start with it — understanding the format makes
everything else obvious.

> **TL;DR**
> - You want to add your own small program to the system → **Path A** (section 3).
> - You want to write a **windowed** application (GUI/NanWM) → **section 3.8**.
> - You want to run an existing Linux program → **Path B** (section 4).
> - You want to understand how it all works under the hood → sections 2, 5, 6 (+ `windowing.md` for GUI).

---

## Table of contents

1. [The executable model in a nutshell](#1-the-executable-model-in-a-nutshell)
2. [Toolchain — where the compiler comes from](#2-toolchain--where-the-compiler-comes-from)
3. [Path A: built-in application (`user/*.c`)](#3-path-a-built-in-application-userc)
4. [Path B: porting an external application (nanos-sdk)](#4-path-b-porting-an-external-application-nanos-sdk)
5. [The `.nxe`/`.ndl` format in detail](#5-the-nxendl-format-in-detail)
6. [How the kernel loads and runs a program](#6-how-the-kernel-loads-and-runs-a-program)
7. [Where the application lands on disk](#7-where-the-application-lands-on-disk)
8. [The API available to applications (libc + syscalls)](#8-the-api-available-to-applications-libc--syscalls)
9. [Debugging and common problems](#9-debugging-and-common-problems)
10. [Checklists](#10-checklists)
11. [Reference file map](#11-reference-file-map)

---

## 1. The executable model in a nutshell

NanOS **does not use ELF at runtime**. A program compiles normally to ELF, after which the
**`mknx`** tool converts it to its own `.nxe` format (Nano eXecutable). A shared library is
saved as `.ndl` (Nano Dynamic Library). It is one format with three variants:

| Extension | What it is               | Flag |
|--------------|---------------------------|-------|
| `.nxe`       | Program (executable)      | —     |
| `.ndl`       | Shared library            | `NX_FLAG_DLL` |
| `.nkext`     | Kernel module (ring 0; see `kext.md`) | — |

The linking model is **Windows PE / MinGW style**, not ELF/glibc:

- The program **imports functions by name** from named libraries (`libc.ndl`). The loader
  patches each IAT (Import Address Table) slot with the address resolved in the library.
- The library **exports functions by name**.
- A module is linked at a "preferred" base, but can load anywhere —
  the loader adds the delta `(real_base − preferred_base)` to each address from the
  relocation table (`R_386_32`).

The consequence is crucial: **a binary depends on a stable, named API, not on a specific
version of a library compiled in hard.** One binary runs on any build that
exports that API — no "a binary for every distribution" problem.

Layout of a `.nxe` file (and of the in-memory image when loaded at `loadBase`):

```
[ NxHeader | code | rodata | data | NxImport[] | NxExport[] | NxReloc[] | NxNeeded[] | strings | (bss — not written) ]
```

Struct details → section 5.

---

## 2. Toolchain — where the compiler comes from

NanOS is a **cross-compiled** project: the host is macOS, but the code targets i686 (32-bit
protected mode). The entire build happens in Docker containers; natively on macOS only
QEMU runs.

There are **two different compilers** for the two paths:

### Path A — containerized `i686-elf` + picolibc

- The `nanos-build` image (`docker/Dockerfile`) builds the `i686-elf` toolchain from source
  (binutils + gcc) plus **picolibc** installed in `/opt/picolibc/i686-elf`.
- This is the compiler the `Makefile` uses to build all built-in programs from `user/*.c`.
- The cross-prefix is the `$(CROSS)` variable, picolibc is `PICOLIBC=/opt/picolibc/i686-elf`.

### Path B — native cross-target `i686-nanos` (nanos-sdk)

- A separate repo **`nanos-sdk`** (`git@github.com:johniak/nanos-sdk.git`, locally in
  `~/Projects/nanos-sdk`) builds a **real cross-target** `i686-nanos`: patched
  binutils + gcc that understand the `nanos` target system.
- `i686-nanos-gcc -dumpmachine` → `i686-nanos`; `__nanos__` / `__NanOS__` are defined;
  `./configure --host=i686-nanos` works out-of-the-box.
- This is the toolchain for **porting existing applications** (autotools/cmake/meson/make).

The bridge between them: NanOS is the source of truth for headers and `libc.ndl`. The SDK
sysroot is refreshed from a NanOS build (the `sync-sysroot` script, and in practice the
`make ping`/`wget`/… targets copy `libc.ndl{,.a}` + headers into the toolchain before building
a port — section 4.6).

---

## 3. Path A: built-in application (`user/*.c`)

The simplest way to a new program in NanOS. You write `user/myapp.c`, add a few lines to
the `Makefile`, run `make image`, and `myapp` is on disk.

### 3.1 Minimal example step by step

**Step 1 — write the program.** File `user/hello.c`:

```c
#include <stdio.h>

int main(int argc, char** argv) {
	printf("hello from NanOS\n");
	if (argc > 1)
		printf("first arg: %s\n", argv[1]);
	return 0;
}
```

This is plain C with picolibc. `printf`, `open`, `read`, `malloc`, `getenv` — everything works,
because it will be imported from `libc.ndl` at runtime.

**Step 2 — register the program in the `Makefile`.** Add the name to the list of programs. In the
userland section (`USER_PROGS`) add `hello`, and to `SYS_PROGS` (if it is a system utility,
it lands in `/nanos/bin`) **or** to `APP_PROGS` (if it is an "application", it lands in the
`/apps/<name>` bundle):

```makefile
# was:
USER_PROGS=init nsh cat ls ... dhcpcfg
# add hello:
USER_PROGS=init nsh cat ls ... dhcpcfg hello

# and e.g. as a system utility:
SYS_PROGS=nsh cat ls free ... hello
```

**Step 3 — add a linking rule.** The generic rule `%.o: user/%.c` will compile
`hello.c` automatically, but linking the `.nxe` requires specifying which objects make up the
program. For a simple program that is just the startup glue + your own `.o`:

```makefile
$(BINFOLDER)hello.nxe: $(USER_GLUE) $(BINFOLDER)hello.o $(BINFOLDER)libc.ndl.a
```

where `$(USER_GLUE)` is the shared startup (`crt0.o sigtramp.o nxhdr.o syscalls.o cwd.o`), and
`libc.ndl.a` is the **import library** (thunks to libc functions). The pattern rule `%.nxe` does
the rest (see below).

**Step 4 — build the image and run:**

```sh
make image        # compiles userland, builds the ext4 image, writes the files
make run          # boots in QEMU
```

In the system: `hello` (via the `/bin` link farm) or with the full path.

### 3.2 What exactly the build does (flags, rules)

Everything below is real `Makefile` fragments.

**Paths and compilation flags:**

```makefile
PICOLIBC=/opt/picolibc/i686-elf

USER_CFLAGS=-ffreestanding -isystem $(PICOLIBC)/include -iquote kernel \
  -Iuser -Iuser/libnw -Iuser/nwm -Iuser/libnwui -Iuser/term \
  -Iuser/libc-glue/include -I$(SBASE) -D_DEFAULT_SOURCE \
  -include user/libc-glue/compat-decls.h -Wall -fno-pic -fno-stack-protector $(UOPTFLAGS)

USER_LIBS=-L$(PICOLIBC)/lib -lc -lgcc
```

- `-isystem $(PICOLIBC)/include` — picolibc headers.
- `-iquote kernel` — `SyscallNr.h` (syscall numbers, shared with the kernel).
- `-Iuser/libc-glue/include` — NanOS POSIX headers (dirent, netinet, sys/*, etc.).
- `-include user/libc-glue/compat-decls.h` — forced declarations missing in picolibc.
- `-fno-pic -fno-stack-protector` — fixed base, no stack protector.
- `$(UOPTFLAGS)` = `-O2 -fno-strict-aliasing -fno-delete-null-pointer-checks`.

**Shared startup (linked into every program):**

```makefile
USER_GLUE=$(BINFOLDER)crt0.o $(BINFOLDER)sigtramp.o $(BINFOLDER)nxhdr.o \
          $(BINFOLDER)syscalls.o $(BINFOLDER)cwd.o
```

**Compilation rule** — programs from `user/*.c` get the `nx-dllimport.h` header force-included
(it redirects data symbols like `stdout`/`errno` through IAT slots — see 3.5):

```makefile
DYNHDR=-include user/libc-glue/nx-dllimport.h

$(BINFOLDER)%.o: user/%.c
	@mkdir -p $(BINFOLDER)
	$(CXX) $(USER_CFLAGS) $(DYNHDR) -MMD -MP -c $< -o $@
```

(The glue code from `user/libc-glue/*.c` is compiled **without** `DYNHDR`, because it *is* libc.)

**The `.nxe` linking rule:**

```makefile
$(BINFOLDER)%.nxe: $(MKNX)
	$(LD) -nostdlib -Wl,--emit-relocs -T user/nx.ld -o $(@:.nxe=.elf) \
	  $(filter %.o,$^) $(filter %.a,$^) -lgcc
	$(MKNX) $(@:.nxe=.elf) $@ --need libc.ndl
```

- `-nostdlib` — no startup/library from the compiler (we have our own `crt0`).
- `-Wl,--emit-relocs` — keep the `R_386_32` relocations (mknx builds the relocation table from them).
- `-T user/nx.ld` — link at base `0x800000` (script below).
- `--need libc.ndl` — record in the header that the program needs `libc.ndl`.

### 3.3 The `user/nx.ld` linker script

Full content (programs, base `0x800000`, the `.nxheader` header first):

```ld
ENTRY(_start)
SECTIONS
{
  . = 0x800000;
  .nxheader : { KEEP(*(.nxheader)) }
  .text     : { *(.text*) }
  .nximports : { *(.nximports) }
  .rodata   : { *(.rodata*) }
  .data     : { *(.data*) }
  /* IAT slots from the import-library — a separate section PER library, so the name .nxlib.<soname>
     survives linking (mknx reads it to assign an import to a library). Add
     a line here when introducing a new shared library. */
  .nxlib.libc.ndl  : { *(.nxlib.libc.ndl) }
  .nxlib.greet.ndl : { *(.nxlib.greet.ndl) }
  __bss_start = .;
  .bss      : { *(.bss*) *(COMMON) }
  __bss_end = .;
  __nx_image_size = __bss_start - 0x800000;
}
```

> **Note:** if you introduce a new `.ndl` library that the program is to use, add
> a line `.nxlib.<soname> : { *(.nxlib.<soname>) }` here.

### 3.4 The `user/crt0.S` startup

The kernel enters `_start` with `esp` pointing at the argv+envp image (SysV i386). `crt0`
publishes the environment to libc, sets `progname`, calls `main`, then `exit`:

```nasm
[BITS 32]
[GLOBAL _start]
[EXTERN main]
[EXTERN exit]
[EXTERN __nx_set_environ]
[EXTERN __nx_set_progname]

_start:
    mov ebp, 0
    mov eax, [esp]              ; argc
    lea edx, [esp + 4]         ; argv
    lea ecx, [esp + 8 + eax*4] ; envp  (= esp + 4 + (argc+1)*4)
    push ecx                    ; main arg3: envp
    push edx                    ; main arg2: argv
    push eax                    ; main arg1: argc
    push ecx                    ; __nx_set_environ(envp)
    call __nx_set_environ       ; make the environment available for getenv()
    add esp, 4
    mov eax, [edx]              ; argv[0]
    push eax                    ; __nx_set_progname(argv[0]) — for getprogname()
    call __nx_set_progname
    add esp, 4
    call main                   ; main(argc, argv, envp)
    add esp, 12
    push eax                    ; main's exit code
    call exit                   ; does not return (the loader longjmps to the kernel)
.hang:
    jmp .hang
```

### 3.5 `libc-glue` and the magic of `nx-dllimport.h`

`user/libc-glue/` is the layer connecting picolibc with the NanOS kernel (this code ultimately
lives **inside `libc.ndl`**). The most important files:

| File                 | Role |
|----------------------|------|
| `syscalls.c`         | syscall wrappers (`int 0x80`): open/read/write/fork/execve/… |
| `cwd.c`              | current directory, path resolution |
| `sigtramp.S/.c`      | signal trampoline |
| `termios.c`, `ptyutil.c` | terminals / PTY |
| `dirent.c`, `pwd_grp.c`  | directories, account database |
| `sockets.c`, `resolv*.c` | sockets + DNS resolver |
| `posixstubs.c`       | stubs (getrlimit/getrusage…) |
| `include/`           | POSIX headers (arpa, net, netinet, sys, dirent.h, netdb.h, poll.h, pty.h…) |

**Why `nx-dllimport.h`?** picolibc exposes `stdin/stdout/stderr` as *data objects*,
and `errno` as a *variable*. The program refers to them by address — which a shared library
cannot satisfy without dllimport-style indirection. Force-including `nx-dllimport.h` redefines
each of these symbols to a dereference of an IAT slot (`__imp_<name>`), which the loader fills
with the address of the symbol inside `libc.ndl` — exactly the `__declspec(dllimport)` model from Windows:

```c
extern FILE **__imp_stdin;
extern FILE **__imp_stdout;
extern FILE **__imp_stderr;
extern int   *__imp_errno;
extern char ***__imp_environ;

#define stdin   (*__imp_stdin)
#define stdout  (*__imp_stdout)
#define stderr  (*__imp_stderr)
#define errno   (*__imp_errno)
#define environ (*__imp_environ)
```

That is why programs are compiled with `DYNHDR`, and the glue (which *is* libc) — without.

### 3.6 How `.ndl` libraries are built

The `.ndl` rule differs only in the linker script (`user/dll.ld`, base `0x09000000`) and the
`mknx --dll` flags. Example of the demo library `greet.ndl`:

```makefile
$(BINFOLDER)greet.ndl: $(BINFOLDER)nxhdr.o $(BINFOLDER)greet.o $(MKNX)
	$(LD) -nostdlib -Wl,--emit-relocs -T user/dll.ld -o $(BINFOLDER)greet.elf \
	  $(BINFOLDER)nxhdr.o $(BINFOLDER)greet.o
	$(MKNX) $(BINFOLDER)greet.elf $@ --dll --export nx_greet --export nx_greeting
```

`libc.ndl` is produced analogously, but with `--export-all`, and additionally an
**import library** `libc.ndl.a` is generated (`mknx … --implib --export-all --soname libc.ndl`), which for
each export produces a thunk `name: jmp [__imp_name]` + an IAT slot in the `.nxlib.libc.ndl` section.
The program links against `libc.ndl.a`, so it pulls in only the symbols actually used.

### 3.7 `user/init.c` — PID 1 (context)

`init` is the first userland program. Its `main` in brief:

```c
int main(void) {
	run_dhcp();        /* bring up eth0 via DHCP (kernel static = fallback) */
	start_services();  /* fork+exec inetd + darkhttpd */
	struct passwd* pw = getpwuid(getuid());
	const char* shell = (pw && pw->pw_shell && pw->pw_shell[0]) ? pw->pw_shell : FALLBACK_SHELL;
	/* … exec the shell, taking over PID 1 … */
}
```

The default shell comes from the 7th field of `/nanos/config/passwd` — editing this file changes
the default shell.

### 3.8 A windowed application (NanWM)

A GUI application is **the same built-in `.nxe` program** as above — except that instead of writing
to the console, it is a **client of the NanWM compositor** (`nwm`). The compositor already exists;
your app connects to it over an inherited pair of pipes (fd 3/4), creates a window, draws pixels and
receives events. The full architecture (compositor, protocol, full API) is described in
**`windowing.md`** — here is just the "how to write an app" recipe.

**Pick a library:**

| Library | When | Examples |
|------------|-------|-----------|
| **`libnwui.ndl`** (toolkit) | a typical app: buttons, lists, text fields, layout, menus | `nwform`, `nwexp`, `nwset`, `nwabout` |
| **`libnw.ndl`** (raw) | custom rendering: you draw pixels into the window buffer and handle events manually | `nwnote`, `nwterm` |

#### Variant A — toolkit (`libnwui`, recommended)

You build a tree of widgets, attach callbacks, `nwui_run()` does the event loop. File
`user/mygui/mygui.c` (modeled on `nwform`):

```c
#include "nwui.h"

static void on_click(nwui_node *self, void *user) {
    nwui_set_text((nwui_node *) user, "Kliknięto!");
}

int main(void) {
    nwui *u = nwui_open("MyGUI", 320, 180);     /* connect + create_window for you */
    if (!u) return 1;
    nwui_node *out = nwui_label(u, "");
    nwui_set_root(u, nwui_pad(nwui_column(u,
        nwui_label(u, "Witaj w NanWM"),
        nwui_button(u, "Naciśnij", on_click, out),
        out,
        (nwui_node *) 0), 12));
    nwui_run(u);                                 /* loop until the window is closed */
    return 0;
}
```

#### Variant B — raw (`libnw`)

You get the window's pixel buffer (BGRX, `0x00RRGGBB`), draw with `nw_gfx` primitives,
`nw_commit()` pushes the damaged rectangle, and `nw_next_event()` is your
`GetMessage`/`DispatchMessage` (modeled on `nwnote`):

```c
#include "libnw.h"
#include "nw_gfx.h"

int main(void) {
    nw_display *d = nw_connect();
    nw_win *win = nw_create_window(d, 360, 220, "note");
    struct nw_surface s; nw_win_surface(win, &s);
    nw_fill_rect(&s, 0, 0, s.w, s.h, 0x00f4f4ec);
    nw_text(&s, 8, 8, "hello", 0x00101014);
    nw_commit(win, 0, 0, s.w, s.h);
    struct nw_event ev;
    while (nw_next_event(d, &ev, -1) == 1) {
        if (ev.type == NW_EV_CLOSE) break;
        /* NW_EV_KEY / NW_EV_POINTER / NW_EV_FOCUS / NW_EV_PASTE / NW_EV_MENU ... */
    }
    return 0;
}
```

#### Build — `Makefile` rules

GUI apps do **not** use the generic `%.nxe` rule (which links only `libc.ndl`). They have
**their own linking rule**: they add the toolkit/client import-library and declare
`--need libnwui.ndl` (or `--need libnw.ndl`). The recursive loader pulls in the rest of the
chain itself (`libnwui → libnw → libc`), the way a Windows app linking `user32` gets `ntdll`.

```makefile
# 1) program list + classification as an APPLICATION (bundle /apps + symlink /bin)
USER_PROGS=... mygui
APP_PROGS=...  mygui

# 2) compilation rule for the app's directory (each GUI app has its own subdirectory user/<name>/)
$(BINFOLDER)%.o: user/mygui/%.c
	@mkdir -p $(BINFOLDER)
	$(CXX) $(USER_CFLAGS) $(DYNHDR) -MMD -MP -c $< -o $@

# 3a) Variant A (toolkit): link libnwui.ndl.a + libc.ndl.a, --need libnwui.ndl
$(BINFOLDER)mygui.nxe: $(DYN_GLUE) $(BINFOLDER)mygui.o $(BINFOLDER)libnwui.ndl.a \
                       $(BINFOLDER)libc.ndl.a $(BINFOLDER)libnwui.ndl $(BINFOLDER)libnw.ndl \
                       $(BINFOLDER)libc.ndl $(MKNX)
	$(LD) -nostdlib -Wl,--emit-relocs -T user/nx.ld -o $(BINFOLDER)mygui.elf \
	  $(DYN_GLUE) $(BINFOLDER)mygui.o $(BINFOLDER)libnwui.ndl.a $(BINFOLDER)libc.ndl.a -lgcc
	$(MKNX) $(BINFOLDER)mygui.elf $@ --need libnwui.ndl

# 3b) Variant B (raw): replace libnwui -> libnw everywhere above and --need libnw.ndl
```

> `libnwui.ndl`/`libnw.ndl`/`libc.ndl` as prerequisites ensure the libraries are built
> and land in `/nanos/lib`. The libraries themselves are built by the rules from section 3.6.

#### Running

```sh
make image && make run
```

In the system, first launch the compositor: `nwm`. It starts the desktop (Terminal + Settings + Files) and
takes over the screen. You run your app:

- from the bar/shortcut **Super+R** (Run) by typing the name, or
- programmatically: `nwui_spawn(u, "mygui")` / `nw_spawn(d, "mygui")` from another app, or
- to have it start together with the desktop — add `spawn_client(slot, "/disks/main/apps/mygui/mygui.nxe")`
  in `user/nwm/nwm.c` (like `nwterm`/`nwset`/`nwexp`).

The app — being in `APP_PROGS` — lands in the bundle `/apps/mygui/mygui.nxe` with a symlink `/bin/mygui.nxe`,
so it works "by name".

#### Another language (Rust)

The `libnwui` ABI is pure C (opaque handles, POD, callbacks), so a GUI app can be written in
any language with a C FFI. `rustform` is exactly the same form as `nwform`, but in Rust:
`cargo` builds a `no_std` staticlib for the `i686-nanos.json` target (`-Z build-std=core,alloc`), and
it is linked with `crt0` + `libnwui.ndl.a` + `libc.ndl.a` and `mknx --need libnwui.ndl` like any
app (see the `rustform` rule in the `Makefile`).

---

## 4. Path B: porting an external application (nanos-sdk)

When you want to run an **existing** program (GNU/BSD, autotools/cmake/meson/make) without
rewriting it — you use `nanos-sdk`. The idea: the entire port is a small **`nxport.toml`** file + (sometimes)
a couple of hooks; the `nanos-port` driver fetches the sources, configures for `i686-nanos`, builds and
passes the result through `mknx`.

### 4.1 The official recipe (`nanos-sdk/docs/PORTING.md`)

```
1. Fork the application's repo (e.g. vim-nanos) with upstream sources (or fetch them via `source`).
2. Add nxport.toml.
3. Build: `nanos-port .` in the nanos-sdk container (toolchain on PATH, sysroot synced
   from a finished NanOS via `sync-sysroot --nanos=/path`). Produces <name>.nxe + <name>.install.
4. In NanOS: a dedicated target (e.g. `make ping`) builds the port and copies <name>.nxe to bin/;
   `make image` installs it.
```

The toolchain is a real cross-target: `i686-nanos-gcc -dumpmachine` → `i686-nanos`, `__nanos__`
is defined, `./configure --host=i686-nanos` accepted right away.

### 4.2 The `nxport.toml` format — all fields

| Field             | Type           | Default         | Meaning |
|-------------------|----------------|-----------------|-----------|
| `name`            | string         | (required)      | application name → `.nxe` file |
| `source`          | string         | (required)      | `git:URL[@ref]`, `tar:URL`, or `dir:path` |
| `build`           | string         | `"autotools"`   | `autotools` / `cmake` / `meson` / `make` |
| `configure`       | array[string]  | `[]`            | flags passed to configure/cmake/meson/make |
| `cache`           | array[string]  | `[]`            | autoconf answers for cross-compile (appended to `config.cache`) |
| `make_target`     | string         | (optional)      | a specific make target (default: all) |
| `needs`           | array[string]  | `["libc.ndl"]`  | `.ndl` libraries → `mknx --need <name>` |
| `binary`          | string         | `<name>`        | path to the linked ELF relative to the sources |
| `data`            | array[string]  | `[]`            | data files: `["src/path -> /target/path"]` |
| `install`         | string         | `/apps/<name>`  | location in the NanOS image (or `sysroot` for a library port) |
| `sysroot_libs`    | array[string]  | (optional)      | libraries to install into the sysroot (when `install="sysroot"`) |
| `sysroot_headers` | array[string]  | (optional)      | headers to install into the sysroot (when `install="sysroot"`) |

The special value `install = "sysroot"` → a library port (installs into the toolchain sysroot,
does not produce a `.nxe`).

### 4.3 The smallest possible port — the `hello` example

From `nanos-sdk/examples/hello/`:

`nxport.toml`:
```toml
name = "hello"
source = "dir:src"
build = "make"
install = "/apps/hello"
```

`src/hello.c`:
```c
#include <stdio.h>
int main(void){ printf("hello from i686-nanos\n"); return 0; }
```

`src/Makefile`:
```makefile
hello: hello.c
	$(CC) -O2 -o hello hello.c
```

`$(CC)` resolves to `i686-nanos-gcc`, which itself appends `crt0.o`, `nxhdr.o`, `nx.ld` and
`--emit-relocs` (via the spec in `toolchain/nanos.h`).

### 4.4 Real examples (autotools)

**`ping` (GNU inetutils)** — `~/Projects/nanos-sdk-work/inetutils-port/nxport.toml`:

```toml
name = "ping"
source = "dir:inetutils-2.5"
build = "autotools"
configure = [
  "--disable-servers", "--disable-clients", "--enable-ping",
  "--disable-rpath", "--without-libidn2", "--without-libcrypt", "--disable-ipv6",
]
cache = [
  "ac_cv_func_select=yes",
  "gl_cv_func_select_supports0=yes",
  "ac_cv_func_getaddrinfo=yes",
  "ac_cv_func_socket=yes",
  "ac_cv_func_connect=yes",
  # … (dozens of answers driving gnulib — see the original file)
]
needs = ["libc.ndl"]
binary = "ping/ping"
install = "/nanos/bin"
```

**`wget` (HTTP-only, no TLS)** — key fields:

```toml
name = "wget"
source = "dir:wget-1.21.4"
build = "autotools"
configure = [
  "--without-ssl", "--disable-ipv6", "--disable-nls", "--without-libpsl",
  "--without-libidn", "--without-zlib", "--disable-pcre", "--disable-pcre2",
  "--without-metalink", "--without-cares",
]
cache = [ /* the same gnulib forcings + stdio-ext + link/pathconf */ ]
needs = ["libc.ndl"]
binary = "src/wget"
install = "/nanos/bin"
```

> **Why so many entries in `cache`?** When cross-compiling, `configure` cannot run target
> binaries, so runtime tests must be "answered" up front. The base answers (type
> sizes, endianness) are in `nanos-sdk/port/config.cache`; the port appends its own. A lot of entries
> force gnulib to use its own GNU implementations where picolibc has a BSD variant.

### 4.5 Non-autotools examples and hooks

**`darkhttpd` (plain make):**

```toml
name = "darkhttpd"
source = "dir:darkhttpd"
build = "make"
configure = ["darkhttpd", "CFLAGS=-O2 -DNO_IPV6"]
needs = ["libc.ndl"]
binary = "darkhttpd"
install = "/nanos/bin"
```

**`inetd` with a hook (many binaries from one build).** The manifest builds `inetd`, and
`hooks/post_build.sh` extracts additional binaries (`telnetd`, `telnet`, `ifconfig`, `traceroute`)
from the same tree:

```sh
#!/bin/sh
mknx_if() {   # $1 = built ELF, $2 = .nxe name
  if [ -f "$1" ]; then
    i686-nanos-mknx "$1" "$PORT/$2" --need libc.ndl && echo "== produced $PORT/$2 =="
  fi
}
mknx_if "$STAGE/telnetd/telnetd"    telnetd.nxe
mknx_if "$STAGE/telnet/telnet"      telnet.nxe
mknx_if "$STAGE/ifconfig/ifconfig"  ifconfig.nxe
mknx_if "$STAGE/src/traceroute"     traceroute.nxe
```

**Hooks** (run if they exist in `hooks/`):

| Hook                  | When |
|-----------------------|-------|
| `pre_configure.sh`    | after fetching the sources, before configure |
| `post_configure.sh`   | after configure, before the build |
| `post_build.sh`       | after the build, before/instead of mknx |

Hook environment: `$STAGE` (the sources directory, e.g. `/tmp/port-wget`), `$PORT` (the port directory).

### 4.6 How `nanos-port` builds each system

The driver (`nanos-sdk/port/nanos-port`, Python 3) for each `build`:

- **autotools:** merges the base `config.cache` with the `cache` from the manifest, then
  `./configure --host=i686-nanos --cache-file=<merged>` + flags, then `make -j`.
- **cmake:** `cmake -DCMAKE_TOOLCHAIN_FILE=…/toolchain-nanos.cmake <flags> ..` in `build-nanos/`, then `make -j`.
- **meson:** `meson setup --cross-file …/nanos-cross.meson <flags> build-nanos`, then `ninja -C`.
- **make:** `make CC=i686-nanos-gcc -j <flags>`.

CMake cross-file (`port/toolchain-nanos.cmake`):
```cmake
set(CMAKE_SYSTEM_NAME nanos)
set(CMAKE_SYSTEM_PROCESSOR i686)
set(CMAKE_C_COMPILER   i686-nanos-gcc)
set(CMAKE_CXX_COMPILER i686-nanos-g++)
set(CMAKE_AR           i686-nanos-ar)
set(CMAKE_RANLIB       i686-nanos-ranlib)
set(CMAKE_CROSSCOMPILING TRUE)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
```

Meson cross-file (`port/nanos-cross.meson`):
```ini
[binaries]
c = 'i686-nanos-gcc'
cpp = 'i686-nanos-g++'
ar = 'i686-nanos-ar'
strip = 'i686-nanos-strip'
pkg-config = 'i686-nanos-pkg-config'

[host_machine]
system = 'nanos'
cpu_family = 'x86'
cpu = 'i686'
endian = 'little'
```

**Honest conftest.** `i686-nanos-gcc` is wrapped by a script: normally it links with
`--unresolved-symbols=ignore-all` (so that stock code can refer to DATA from `libc.ndl`,
which `mknx` turns into auto-imports). But for the autoconf trial program (`conftest`) the wrapper
adds `--unresolved-symbols=report-all` + a data stub, so that `AC_CHECK_FUNC` **honestly**
detects missing functions (otherwise every function "exists" and gnulib generates wrong
`HAVE_<fn>=1`).

### 4.7 Running a port from within NanOS

Ports are driven by **dedicated, repeatable targets** in the NanOS `Makefile` (not a generic
`make port APP=`). Each target:

1. checks that the SDK toolchain exists (`$(SDK_TC)/i686-nanos/include`),
2. **refreshes the sysroot from THIS checkout** (copies `user/libc-glue/include/.`, `SyscallNr.h`,
   `libc.ndl.a` → `libc.a`, `libc.ndl`),
3. runs `nanos-port` in the `nanos-sdk-dev` container,
4. copies the resulting `.nxe` into `bin/`.

A real example (`make ping`):

```makefile
NANOS_SDK ?= $(HOME)/Projects/nanos-sdk
SDK_WORK  ?= $(HOME)/Projects/nanos-sdk-work
SDK_TC    := $(SDK_WORK)/toolchain
PING_PORT := $(SDK_WORK)/inetutils-port

ping: bin/libc.ndl bin/libc.ndl.a
	@test -d "$(SDK_TC)/i686-nanos/include" || { echo "nanos-sdk toolchain not found"; exit 1; }
	cp -R user/libc-glue/include/. "$(SDK_TC)/i686-nanos/include/"
	cp kernel/SyscallNr.h          "$(SDK_TC)/i686-nanos/include/SyscallNr.h"
	cp $(BINFOLDER)libc.ndl.a      "$(SDK_TC)/i686-nanos/lib/libc.a"
	cp $(BINFOLDER)libc.ndl        "$(SDK_TC)/i686-nanos/lib/libc.ndl"
	docker run --rm \
	  -v "$(SDK_TC)":/work/toolchain -v "$(PING_PORT)":/work/port -v "$(NANOS_SDK)":/sdk \
	  -e SDK=/sdk -e PATH="/work/toolchain/bin:…:/bin" \
	  -w /work/port nanos-sdk-dev:latest python3 /sdk/port/nanos-port /work/port
	cp "$(PING_PORT)/ping.nxe" $(BINFOLDER)ping.nxe
```

Existing targets: `make ping`, `make wget`, `make inetd`, `make httpd`, `make udhcpc`
(repeatable, via nanos-port); `make grep`, `make vim`, `make bzip2`, `make bash`
(copy/build from their own trees); `make externals` (bulk-copies everything already built).
After each of them → `make image`.

> **Adding a new port** boils down to: (a) a directory in `$(SDK_WORK)/<app>-port/` with
> `nxport.toml` (+ optionally `hooks/`), (b) copying the target pattern in the `Makefile` with the names swapped.

### 4.8 Building the toolchain and sysroot (one-time)

- `nanos-sdk/build-toolchain.sh` builds binutils (2.43) + gcc (14.2.0) for `i686-nanos` into
  `/opt/i686-nanos` (the patches from `toolchain/` teach them the `nanos` system; `toolchain/nanos.h` is
  the gcc target-config defining `STARTFILE_SPEC`, `LINK_SPEC`, `__nanos__`).
- `nanos-sdk/sysroot/sync-sysroot --nanos=/path/NanOS` fills the sysroot: picolibc headers +
  glue + `SyscallNr.h`, `libc.ndl.a`→`libc.a`, `crt0.o`, `nxhdr.o`, `nx.ld`, `libc.ndl`, and
  compiles `i686-nanos-mknx` from `tools/mknx.c`. The whole machinery is frozen in the
  `nanos-sdk-dev` image.

---

## 5. The `.nxe`/`.ndl` format in detail

From `kernel/NxFormat.h` (shared between the kernel and `tools/mknx.c`):

```c
#define NX_MAGIC    0x0045584E   /* 'N','X','E',0 little-endian */
#define NX_VERSION  3
#define NX_FLAG_DLL 1u           /* bit 0 flags: the module is a shared library */

typedef struct {                 /* import: resolve nameOff in library libOff, write into slotAddr */
	unsigned nameOff;            /* address of the import name */
	unsigned slotAddr;           /* address of the IAT slot to patch */
	unsigned libOff;             /* address of the source library name, or 0 = flat */
} NxImport;

typedef struct {                 /* export: nameOff callable at addr after loading */
	unsigned nameOff;
	unsigned addr;
} NxExport;

typedef struct {                 /* base relocation: the 32-bit word at off gets the load delta */
	unsigned off;
} NxReloc;

typedef struct {                 /* needed library: load the .ndl named nameOff before the imports */
	unsigned nameOff;
} NxNeeded;

typedef struct {
	unsigned magic, version, flags;
	unsigned entry;              /* entry address (.nxe) */
	unsigned loadBase;           /* preferred linking base */
	unsigned imageSize;          /* bytes written in the file */
	unsigned bssStart, bssEnd;   /* zeroed by the loader */
	unsigned importTable, importCount;
	unsigned exportTable, exportCount;
	unsigned relocTable,  relocCount;
	unsigned neededTable, neededCount;
} NxHeader;
```

**`mknx` (CLI)** — `tools/mknx.c`, built by `cc -O2 -Wall -Ikernel -o bin/mknx tools/mknx.c`:

```
mknx <in.elf> <out.nxe|out.ndl> [--dll] [--export NAME]... [--export-all] [--need NAME]...
mknx <in.elf> <out.s>           --implib [--export NAME]... [--export-all] [--soname NAME]
```

What it does: reads ELF32 with relocations, determines `loadBase`/`bss`, copies the allocated sections into
a contiguous image, collects `R_386_32` relocations, builds the import/export/needed tables, glues
everything together with the header (the addresses in the header are absolute). In `--implib` mode it generates a directory with
a `.s` per export (thunk + IAT slot) → then `nasm` + `ar` produce `libc.ndl.a`.

---

## 6. How the kernel loads and runs a program

- **Program load base:** `0x800000` (`STAGE_BASE` in `kernel/Exec.cpp`, consistent with
  `user/nx.ld`).
- **Libraries** load in the "module band" — `kernel/DynLoader.cpp` allocates successive
  bases every `arch::mmuModuleStride()`.
- **Per-DLL namespace:** each loaded library has its own symbol table (keyed by
  soname); an import points to its library via the `.nxlib.<soname>` section.
- **Recursive loader:** when loading a `.ndl`, it first loads all of its `NEEDED` (post-order),
  so a client linking `libnw.ndl` automatically gets `libc.ndl` (the dyld/Windows model).
- **Relocation:** delta = `real_base − loadBase`; `NxeLoader` applies it to each location from
  `NxReloc[]`.
- **IAT binding:** each `NxImport` slot is patched with the address of the symbol from the target library's table.
- **Entering ring 3:** the kernel creates the process's `AddressSpace`, maps the image + a 512 KiB stack
  (`archLoadUser`) and enters ring 3 via `iret` (`archEnterUser`, selectors `0x1B`/`0x23`).
- **Syscalls and exit:** the program calls the kernel via `int 0x80`; `exit()` ends up in `procExit`,
  which frees the address space and hands control back to the scheduler (the trap-frame model from PHASE 4 —
  **not** setjmp/longjmp).

> A full description of the format and the runtime (the `NxeLoader`/`DynLoader` loaders, the userland memory map,
> the transition to ring 3, fork/execve, signals) → **`nxe-ndl.md`**.

---

## 7. Where the application lands on disk

A full description of the layout → `filesystem.md`. In brief, the build writes the files via `debugfs`:

```
/disks/main/
├── nanos/
│   ├── bin/            SYSTEM utilities (flat): nsh, cat, ls, free, ping, wget, grep…
│   └── lib/            shared libraries: libc.ndl, libnw.ndl, libnwui.ndl…
├── apps/
│   └── <name>/         a NON-system application bundle:
│       ├── <name>.nxe
│       └── …           its own data (e.g. apps/doom/doom1.wad)
└── bin/                LINK FARM: /bin/<name>.nxe -> /apps/<name>/<name>.nxe (symlink)
```

- **`SYS_PROGS`** → `/nanos/bin/<name>.nxe` (a system utility).
- **`APP_PROGS`** → `/apps/<name>/<name>.nxe` + symlink `/bin/<name>.nxe` (an application in a bundle).
- **`USER_LIBS_NDL`** → `/nanos/lib/<name>.ndl`.

The shell (`user/nsh.c`) resolves a bare command name in the order:
`/nanos/bin/<cmd>.nxe` → `/bin/<cmd>.nxe` (link farm) → `/apps/<cmd>/<cmd>.nxe`.

---

## 8. The API available to applications (libc + syscalls)

- **C library:** picolibc (printf/scanf, malloc/free, string.h, math, stdio on files…).
- **POSIX (via glue):** open/read/write/close/lseek, fork/execve/wait, pipe, dup,
  signals (sigaction + trampoline), termios, PTY, dirent (opendir/readdir),
  pwd (getpwuid), BSD sockets (socket/bind/connect/sendto/recvfrom), resolver
  (getaddrinfo/gethostbyname), clock_gettime (monotonic), nanosleep, getrlimit (stub).
- **Syscall numbers:** `kernel/SyscallNr.h` (Linux-style i386, `int 0x80`).
- **GUI:** `libnw.ndl` (the NanWM compositor client) + `libnwui.ndl` (UI toolkit) — how to write
  a windowed app: **section 3.8**; architecture + full API: **`windowing.md`**. Examples:
  `nwnote`/`nwterm` (raw, libnw), `nwform`/`nwexp`/`nwset`/`nwabout` (toolkit, libnwui),
  `rustform` (toolkit in Rust).

What is **missing** (as of today): TLS/OpenSSL (so `wget` is HTTP-only), IPv6, full locale/NLS,
threads. With ports this is reflected in the `configure` flags (`--without-ssl`,
`--disable-ipv6`, `--disable-nls`).

---

## 9. Debugging and common problems

**Path A (built-in):**

- *The case-insensitive macOS trap:* do not rely on `-Ilib` with `<string.h>` — the build copies
  the sources to the container's case-sensitive FS. This is already handled in the Makefile; just remember that
  userland programs use picolibc (`-isystem`), not the kernel headers.
- *Missing `nx-dllimport.h`:* if a program refers to `stdout`/`errno` and you get
  an "undefined reference" at `mknx` — make sure the file is built by the `user/%.c` rule
  (with `DYNHDR`), not as glue.
- *A new `.ndl` library:* add a `.nxlib.<soname>` section in `user/nx.ld`, otherwise imports from
  it will not be tagged.

**Path B (ports):**

- *`configure` hangs / detects functions wrongly:* this is cross-compile — fill in the `cache` in
  `nxport.toml` (model it on `inetutils-port`/`wget-port`). Honest-conftest makes
  missing functions be reported honestly.
- *Missing headers:* add them to `user/libc-glue/include/` in NanOS and rebuild — the porting
  targets refresh the sysroot from this checkout before the build.
- *gcc 14 promotes a warning to an error:* add `CFLAGS=-Wno-error=…` in `configure` (a build flag,
  not a source patch) — see the `inetd` manifest.

**Verification in QEMU (headless):** build the image with `grub.cfg timeout=0`, boot with
`-display none -monitor unix:/tmp/qmon,…`, dump the screen (`screendump`), convert with `sips` and
look at the PNG. To diagnose faults: `-no-reboot -d int -D log`, grep `v=0d`/`v=08`.

---

## 10. Checklists

### A new built-in application (Path A)

- [ ] `user/<name>.c` with `int main(...)`.
- [ ] add `<name>` to `USER_PROGS` in the `Makefile`.
- [ ] add to `SYS_PROGS` (→ `/nanos/bin`) or `APP_PROGS` (→ `/apps/<name>` + link farm).
- [ ] the rule `$(BINFOLDER)<name>.nxe: $(USER_GLUE) $(BINFOLDER)<name>.o $(BINFOLDER)libc.ndl.a`
      (+ extra `.o`, if the program has more files).
- [ ] (if you use a new `.ndl`) a `.nxlib.<soname>` section in `user/nx.ld`.
- [ ] `make image && make run`.

### A new windowed application (NanWM, section 3.8)

- [ ] `user/<name>/<name>.c` with `int main(...)` using `nwui.h` (toolkit) or `libnw.h` (raw).
- [ ] add `<name>` to `USER_PROGS` **and** `APP_PROGS` (a GUI app = a bundle `/apps/<name>`).
- [ ] the compilation rule `$(BINFOLDER)%.o: user/<name>/%.c` (the app has its own subdirectory).
- [ ] **your own** linking rule: `libnwui.ndl.a` (or `libnw.ndl.a`) + `libc.ndl.a`,
      `mknx … --need libnwui.ndl` (or `--need libnw.ndl`) — not the generic `%.nxe`.
- [ ] `make image && make run`, in the system run `nwm`, then the app (Super+R / `spawn`).

### A new port (Path B)

- [ ] a directory `$(SDK_WORK)/<app>-port/` with `nxport.toml`.
- [ ] `source` (`dir:`/`git:`/`tar:`), `build`, `configure`, `binary`, `install`.
- [ ] `cache` with cross-compile answers (copy from an existing port, adjust).
- [ ] optionally `hooks/{pre_configure,post_configure,post_build}.sh`.
- [ ] a target in the NanOS `Makefile` modeled on `ping`/`wget` (refresh the sysroot → `nanos-port` →
      copy `.nxe` to `bin/`).
- [ ] `make <app> && make image && make run`.

---

## 11. Reference file map

**NanOS (`~/Projects/NanOS`):**

| File | What it contains |
|------|------------|
| `Makefile` (userland section ~501–779) | flags, compilation/linking rules, program lists, installation into the image |
| `Makefile` (external section ~47–207) | port targets (`ping`/`wget`/`inetd`/`httpd`/`udhcpc`/`externals`) |
| `user/nx.ld` | program linker script (base `0x800000`) |
| `user/dll.ld` | library linker script (base `0x09000000`) |
| `user/crt0.S` | startup `_start → main → exit` |
| `user/libc-glue/` | the POSIX/syscall layer (libc.ndl code) + headers in `include/` |
| `user/libc-glue/nx-dllimport.h` | redirection of data symbols (stdout/errno/…) through the IAT |
| `user/libc-glue/compat-decls.h` | declarations missing in picolibc |
| `user/libnw/{libnw.h,nwproto.h,nw_gfx.h}` | the NanWM client: window/event API, protocol, rasterizer |
| `user/libnwui/nwui.h` | the UI toolkit (widgets, layout, menus) — pure C ABI |
| `user/nwm/` | the compositor (`nwm.nxe`) |
| `user/{nwform,nwnote,nwexp,nwset,nwabout,nwterm}/` | example windowed apps |
| `user/rust/rustform/` | an example GUI app in Rust (toolkit via C FFI) |
| `docs/windowing.md` | the NanWM architecture + full API (complements section 3.8) |
| `kernel/NxFormat.h` | the `.nxe`/`.ndl` format definition |
| `kernel/Exec.cpp`, `kernel/NxeLoader.cpp`, `kernel/DynLoader.cpp` | loading/running |
| `kernel/SyscallNr.h` | syscall numbers (i386, `int 0x80`) |
| `tools/mknx.c` | the ELF → `.nxe`/`.ndl` converter |
| `user/free.c`, `user/init.c` | simple program examples |
| `docs/filesystem.md` | the filesystem layout (where everything lands) |

**nanos-sdk (`~/Projects/nanos-sdk`):**

| File | What it contains |
|------|------------|
| `README.md` | SDK overview |
| `docs/PORTING.md` | the official port recipe |
| `build-toolchain.sh` | building `i686-nanos` binutils+gcc |
| `toolchain/nanos.h` | the gcc target-config (`STARTFILE_SPEC`/`LINK_SPEC`/`__nanos__`) |
| `port/nanos-port` | the port driver (Python) |
| `port/config.cache` | base cross-compile answers |
| `port/toolchain-nanos.cmake` | the CMake cross-file |
| `port/nanos-cross.meson` | the Meson cross-file |
| `sysroot/sync-sysroot` | filling the sysroot from a NanOS build |
| `examples/hello/` | a minimal port (a pattern to copy) |

**Work/ports (`~/Projects/nanos-sdk-work`):** `inetutils-port`, `wget-port`,
`inetutils-services-port`, `darkhttpd-port`, `busybox-1.36.1`, `grep-3.11`, `vim`, `bzip2-1.0.8`
— real, working manifests to look at.

---

*This document describes the state as of June 2026 (the `dockerized-build` branch). The `.nxe` format = NX_VERSION 3.*

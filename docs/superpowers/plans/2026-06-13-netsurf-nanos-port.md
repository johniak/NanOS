# NetSurf-on-NanOS Port — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship a graphical web browser for NanOS — NetSurf running as a NanWM client (`.nxe`), rendering real CSS 2.1 + images over HTTP/HTTPS, with optional ES5 JavaScript (Duktape) as a later phase.

**Architecture:** All upstream sources are ported in a **separate git repo** (`netsurf-nanos`) using the existing `nanos-sdk` cross-toolchain (`i686-nanos`) and `nanos-port`/`nxport.toml` machinery. Each third-party library is an independent port that installs **into the SDK sysroot** (`install = "sysroot"`); NetSurf itself builds against that sysroot and installs as a `.nxe` bundle into the NanOS image. The graphical integration is a **new `libnsfb` surface backend** that draws into a NanWM window buffer (`nw_surface`, a 32bpp BGRX framebuffer) and translates `nw_next_event` into `nsfb` events — i.e. we retarget NetSurf's framebuffer frontend from Linux fbdev to NanWM. No kernel changes, no threads, no GPU.

**Tech Stack:** C only. nanos-sdk (`i686-nanos-gcc` 14.2). NanOS `libnw.ndl` (NanWM client). OpenSSL 3.0.15 (already ported, `make openssl`) for HTTPS via libcurl. NetSurf libraries (libwapcaplet, libparserutils, libhubbub, libdom, libcss, libnsgif, libnsbmp, libnsutils, libnslog, libnsfb) + external deps (zlib, libpng, libjpeg, expat, libcurl). Optional: Duktape + `nsgenbind` for JS.

---

## How to read this plan (multi-agent execution model)

This plan is built so **multiple agents can work in parallel**. Read this section before taking a task.

### Two kinds of work, two kinds of concurrency

1. **Authoring a port** (writing `ports/<lib>/nxport.toml` + cache forcings + a smoke test) is **fully independent** per library — any number of agents can author different ports at once with zero shared state. Each agent owns one directory under `ports/`.
2. **Building** ports must respect the **dependency order** (a lib installs headers/libs into the shared sysroot that downstream libs consume). Building is therefore **serialized by the orchestrator** (`scripts/build-all.sh`), not by agents. Agents never hand-build into the shared sysroot concurrently — they build their port in **isolation** (a throwaway sysroot copy) to prove it works, then mark the task done; the orchestrator does the real ordered install.

> **Shared-state rule:** the **only** shared mutable state is the SDK sysroot. Agents MUST NOT run `scripts/build-all.sh` (the ordered installer) in parallel. Per-port verification (Task step "build in isolation") uses a **private sysroot copy** so two agents never write the same sysroot at once.

### Dependency waves (what can run in parallel)

```
Wave 0 (no deps — all parallel):
  repo scaffold · zlib · libjpeg · expat · libiconv · libwapcaplet
  · libnsgif · libnsbmp · libnsutils · libnslog · libnsfb(core)
  · libcurl(needs OpenSSL, already done) · duktape · nsgenbind(host tool)
  · frontend-nanwm backend (codes against libnsfb-core; testable with a stub)

Wave 1 (need Wave 0):
  libpng(needs zlib) · libparserutils(needs libiconv)

Wave 2 (need Wave 1):
  libhubbub(needs parserutils+wapcaplet) · libcss(needs parserutils+wapcaplet)

Wave 3 (need Wave 2):
  libdom(needs hubbub+parserutils+wapcaplet+expat)

Wave 4:
  netsurf app build, NO JS (needs ALL libs + frontend-nanwm)

Wave 5:
  NanOS image integration (Makefile target + install .nxe)

Wave 6 (optional, after a working browser):
  JS — wire duktape + nsgenbind, rebuild netsurf with JS

Wave 7:
  stabilization (real-page debugging, memory, fonts)
```

### Foundations are done properly — NO SHORTCUTS

The base libraries are the foundation everything else stands on; a corner cut here corrupts every
layer above it and is near-impossible to debug later. **Non-negotiable rules for every base port:**

- **Full, correct feature build — not a stub.** A library is ported with its real functionality
  enabled. Disabling a feature is allowed ONLY for genuine platform scope (e.g. curl's FTP/SMTP —
  NetSurf never uses them) and every disabled option MUST carry a one-line `# why:` comment in the
  `nxport.toml`. "Disabled because it was failing to build" is NOT an acceptable reason — fix the build.
- **No deferred foundational behavior.** Charset/encoding conversion, TLS verification, image
  decoding correctness, integer/endianness config — these are foundational and are done right the
  first time, not "built-in subset now, revisit later". (This is why libiconv is a real port below,
  not a built-in-tables stopgap.)
- **Verify against the upstream test suite, not just a smoke test.** Where a library ships tests
  (zlib `make check`, libpng `pngtest`, libjpeg `tjunittest`, libcurl `make test` subset, expat
  tests, the NetSurf libs' `test/` dirs), the port task RUNS them — host-built where the code is pure
  logic, under QEMU where it needs the target. The smoke test proves it links+loads; the test suite
  proves it's correct. A base port is not "done" until its own tests pass.
- **Cache forcings are derived, not guessed.** Do not ship `cache = []` and "add if it breaks". For
  each autotools port, build, inspect the generated `config.h`/`config.log`, and confirm every
  `HAVE_*`/feature decision is actually correct for NanOS (honest-conftest makes `AC_CHECK_FUNC`
  truthful — use it, then verify). Each forcing added carries a `# why:` note.
- **Complete headers.** `sysroot_headers` lists every public header a consumer includes — verified by
  the consumer compiling clean, not by trial-and-error truncation.

If a base port can't be done properly within a task, STOP and flag it — do not paper over it to move
on. A correct foundation slowly beats a fast foundation that has to be redone.

### Per-port task template (every `ports/<lib>` task follows this)

Each library task has the **same shape**; the per-lib specifics (source URL, build system, configure flags, smoke test) are given in the task. The shape is:

````markdown
**Files:**
- Create: `ports/<lib>/nxport.toml`
- Create: `ports/<lib>/cache.txt`            (cross-compile autoconf answers; empty for non-autotools)
- Create: `smoke/<lib>_smoke.c`              (tiny program exercising one API)
- Create: `ports/<lib>/hooks/*.sh`           (only if the task needs them)

- [ ] **Step 1: Write the smoke test first** — `smoke/<lib>_smoke.c`, a ~15-line program that
      `#include`s the lib's public header and calls one real function, printing a known result.
      (This is the port's "failing test": it cannot link until the lib is built+installed.)
- [ ] **Step 2: Author `nxport.toml`** — fill `name/source/build/configure/cache/install/sysroot_*`.
- [ ] **Step 3: Build in ISOLATION** — `scripts/port-verify.sh <lib>` (copies the sysroot to a
      temp dir, runs `nanos-port` against the copy, builds the smoke test against the copy).
      Reference the EXISTING working ports `~/Projects/nanos-sdk-work/{wget-port,inetutils-port}/`
      for the gnulib cache idiom.
- [ ] **Step 4: Audit the build — cache + config correctness (no guessing).** Open the generated
      `config.h`/`config.log` (or the lib's own config output) and confirm every feature decision is
      actually right for NanOS. Add each needed `cache.txt` forcing with a `# why:` note. Do NOT
      leave `cache = []` "to fill if it breaks" — verify it's correct now.
- [ ] **Step 5: Run the upstream test suite** — where the lib ships one (`make check` / its `test/`
      dir / a named test binary). Host-built where the code is pure logic; under QEMU where it needs
      the target. Expected: the suite passes. (Smoke proves it links; this proves it's correct.)
- [ ] **Step 6: Run the smoke test** — under QEMU via `scripts/smoke-run.sh <lib>` (boots a tiny
      NanOS image that runs `/apps/smoke/<lib>_smoke.nxe` and screendumps stdout). Expected: the
      known result line.
- [ ] **Step 7: Commit** — `git add ports/<lib> smoke/<lib>_smoke.c && git commit -m "port: <lib>"`.
````

Definition of done for a base port = its upstream test suite passes (Step 5) AND the smoke test
prints its expected line under QEMU (Step 6). A library with no upstream tests falls back to the
smoke test plus a documented manual exercise of its real path.

### Reference material (read before authoring any port)

- `~/Projects/NanOS/docs/en/writing-apps.md` §4 — the whole port path, `nxport.toml` fields, the honest-conftest wrapper, `install = "sysroot"`.
- `~/Projects/nanos-sdk/docs/PORTING.md` — the official recipe.
- `~/Projects/nanos-sdk-work/wget-port/` and `inetutils-port/` — **working** autotools ports with full `cache` blocks (copy the gnulib forcings from here).
- NetSurf upstream docs: the `netsurf-all-<ver>` bundle ships `Docs/` + a `buildsystem` + an `env.sh`; the framebuffer frontend lives in `frontends/framebuffer`, the surface backends in `libnsfb/src/surface/` (we mirror `linux.c` for fbdev structure and `sdl.c` for event translation).

---

## Phase 0 — Separate repo + build orchestration

### Task 0.1: Scaffold the `netsurf-nanos` repo

**Files:**
- Create: `~/Projects/netsurf-nanos/.gitignore`
- Create: `~/Projects/netsurf-nanos/README.md`
- Create: `~/Projects/netsurf-nanos/ports/.keep`
- Create: `~/Projects/netsurf-nanos/smoke/.keep`
- Create: `~/Projects/netsurf-nanos/frontend-nanwm/.keep`
- Create: `~/Projects/netsurf-nanos/scripts/.keep`

- [ ] **Step 1: Create and init the repo**

```bash
mkdir -p ~/Projects/netsurf-nanos/{ports,smoke,frontend-nanwm,scripts,docs}
cd ~/Projects/netsurf-nanos
git init
touch ports/.keep smoke/.keep frontend-nanwm/.keep scripts/.keep
```

- [ ] **Step 2: Write `.gitignore`**

```gitignore
/build/
/stage/
/sysroot-*/
*.nxe
*.elf
*.o
*.a
src/
work/
```

- [ ] **Step 3: Write `README.md`**

```markdown
# netsurf-nanos

Port of the NetSurf web browser to NanOS, running as a NanWM client (.nxe).

- `ports/<lib>/`   — one nxport.toml per upstream library (installs into the SDK sysroot)
- `frontend-nanwm/`— our libnsfb surface backend (NanWM ↔ NetSurf framebuffer frontend)
- `smoke/`         — one tiny smoke-test program per library
- `scripts/`       — build orchestration (build-all, port-verify, smoke-run)

Build order and parallelism: see the plan in NanOS `docs/superpowers/plans/2026-06-13-netsurf-nanos-port.md`.

Prereqs: nanos-sdk toolchain at $NANOS_SDK / $SDK_WORK, a built NanOS checkout (for the synced
sysroot + libnw headers), and `make openssl` done (HTTPS).
```

- [ ] **Step 4: Commit**

```bash
git add -A && git commit -m "scaffold netsurf-nanos repo"
```

### Task 0.2: Sysroot sync + isolation wrappers

**Files:**
- Create: `~/Projects/netsurf-nanos/scripts/env.sh`
- Create: `~/Projects/netsurf-nanos/scripts/sync-sysroot.sh`
- Create: `~/Projects/netsurf-nanos/scripts/port-verify.sh`

- [ ] **Step 1: Write `scripts/env.sh`** (single source of paths; sourced by every script)

```bash
#!/bin/sh
# Paths shared by all scripts. Override via environment if your layout differs.
: "${NANOS:=$HOME/Projects/NanOS}"
: "${NANOS_SDK:=$HOME/Projects/nanos-sdk}"
: "${SDK_WORK:=$HOME/Projects/nanos-sdk-work}"
: "${SDK_TC:=$SDK_WORK/toolchain}"             # i686-nanos toolchain prefix
export NANOS NANOS_SDK SDK_WORK SDK_TC
export REPO="$(cd "$(dirname "$0")/.." && pwd)"
```

- [ ] **Step 2: Write `scripts/sync-sysroot.sh`** (refresh the toolchain sysroot from a built NanOS — same step `make ping` does)

```bash
#!/bin/sh
set -e
. "$(dirname "$0")/env.sh"
test -d "$SDK_TC/i686-nanos/include" || { echo "toolchain missing at $SDK_TC"; exit 1; }
test -f "$NANOS/bin/libc.ndl.a" || { echo "build NanOS first (make build_userland / image)"; exit 1; }
cp -R "$NANOS"/user/libc-glue/include/. "$SDK_TC/i686-nanos/include/"
cp "$NANOS"/kernel/SyscallNr.h          "$SDK_TC/i686-nanos/include/SyscallNr.h"
cp "$NANOS"/bin/libc.ndl.a              "$SDK_TC/i686-nanos/lib/libc.a"
cp "$NANOS"/bin/libc.ndl                "$SDK_TC/i686-nanos/lib/libc.ndl"
# OpenSSL headers+libs (from the openssl port) — needed by libcurl/HTTPS
if [ -d "$SDK_WORK/openssl-port/stage" ]; then
  cp -R "$SDK_WORK"/openssl-port/stage/include/. "$SDK_TC/i686-nanos/include/" 2>/dev/null || true
  cp "$SDK_WORK"/openssl-port/stage/lib/lib*.a    "$SDK_TC/i686-nanos/lib/"     2>/dev/null || true
fi
echo "sysroot synced into $SDK_TC/i686-nanos"
```

- [ ] **Step 3: Write `scripts/port-verify.sh`** (build ONE port against a PRIVATE sysroot copy — this is what an agent runs; never touches the shared sysroot)

```bash
#!/bin/sh
# Usage: port-verify.sh <lib>
# Copies the toolchain to a temp dir, builds the port + its smoke test there.
set -e
. "$(dirname "$0")/env.sh"
LIB="$1"; test -n "$LIB" || { echo "usage: port-verify.sh <lib>"; exit 2; }
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
cp -R "$SDK_TC" "$TMP/tc"
docker run --rm \
  -v "$TMP/tc":/work/toolchain \
  -v "$REPO/ports/$LIB":/work/port \
  -v "$NANOS_SDK":/sdk \
  -e SDK=/sdk -e PATH="/work/toolchain/bin:/usr/bin:/bin" \
  -w /work/port nanos-sdk-dev:latest python3 /sdk/port/nanos-port /work/port
echo "== $LIB built OK in isolation =="
```

- [ ] **Step 4: Make executable + commit**

```bash
chmod +x scripts/*.sh
git add scripts && git commit -m "scripts: env + sysroot sync + isolated port verify"
```

### Task 0.3: Smoke harness (run a .nxe under QEMU, capture stdout)

**Files:**
- Create: `~/Projects/netsurf-nanos/scripts/smoke-run.sh`
- Create: `~/Projects/netsurf-nanos/smoke/Makefile`

- [ ] **Step 1: Write `smoke/Makefile`** (links a smoke `.c` into a `.nxe` with crt0 + libc, exactly like a NanOS USER_PROG)

```makefile
# Build one smoke .nxe: make NAME=<lib>  (uses the i686-nanos toolchain)
TC      ?= $(SDK_TC)
CC      := $(TC)/bin/i686-nanos-gcc
MKNX    := $(TC)/bin/i686-nanos-mknx
NAME    ?=
all: $(NAME)_smoke.nxe
%_smoke.nxe: %_smoke.c
	$(CC) -O2 -o $*_smoke.elf $< $(EXTRA_LIBS)
	$(MKNX) $*_smoke.elf $@ --need libc.ndl
```

> `i686-nanos-gcc` appends crt0/nxhdr/nx.ld via its spec (see writing-apps.md §4.3), so a plain
> `gcc -o` link produces a NanOS ELF; `mknx` converts it. `EXTRA_LIBS` carries `-l<lib>` per smoke.

- [ ] **Step 2: Write `scripts/smoke-run.sh`** (build smoke .nxe, drop into a throwaway NanOS image under `/apps/smoke/`, boot headless, screendump)

```bash
#!/bin/sh
# Usage: smoke-run.sh <lib> [EXTRA_LIBS="-lz"]
set -e
. "$(dirname "$0")/env.sh"
LIB="$1"; shift || true
make -C "$REPO/smoke" NAME="$LIB" SDK_TC="$SDK_TC" "$@"
# Install into a copy of the NanOS image and boot headless (reuse NanOS's QEMU smoke pattern).
"$NANOS"/scripts/run-smoke-nxe.sh "$REPO/smoke/${LIB}_smoke.nxe" "/apps/smoke/${LIB}_smoke.nxe"
```

> **Dependency:** this references a small NanOS-side helper `scripts/run-smoke-nxe.sh` that installs
> one `.nxe`, boots `qemu-system-i386 -display none -monitor unix:/tmp/qmon`, runs it from the shell,
> and screendumps. It is created in Task 5.0 (NanOS side). Until then, smoke tests can be run by hand
> with the headless QEMU recipe in `docs/en/writing-apps.md` §9.

- [ ] **Step 3: Make executable + commit**

```bash
chmod +x scripts/smoke-run.sh
git add scripts/smoke-run.sh smoke/Makefile && git commit -m "scripts: QEMU smoke harness"
```

---

## Phase 1 — External dependencies (Wave 0 + Wave 1)

All Phase-1 tasks use the **per-port task template** above. Below are the per-lib specifics. Where a
task abbreviates the tail as "verify, smoke, commit", it means the **full 7-step template** — including
**Step 4 (cache/config audit)** and **Step 5 (run the upstream test suite)**. Those two steps are not
optional for base libraries.

### Task 1.1: Port `zlib` (Wave 0)

**Files:** `ports/zlib/nxport.toml`, `smoke/zlib_smoke.c`

- [ ] **Step 1: Smoke test** `smoke/zlib_smoke.c`

```c
#include <stdio.h>
#include <zlib.h>
int main(void){ printf("zlib %s\n", zlibVersion()); return 0; }
```

- [ ] **Step 2: `nxport.toml`** — zlib uses its own `./configure` (not autotools-gnulib):

```toml
name = "zlib"
source = "tar:https://zlib.net/zlib-1.3.1.tar.gz"
build = "make"
configure = ["CC=i686-nanos-gcc", "AR=i686-nanos-ar", "prefix=/usr"]
# zlib's configure is custom; if it misdetects, use a pre_configure hook to run
# `CHOST=i686-nanos ./configure --static --prefix=/usr` instead of build="make".
install = "sysroot"
sysroot_libs = ["libz.a"]
sysroot_headers = ["zlib.h", "zconf.h"]
```

- [ ] **Step 3–5:** verify (`scripts/port-verify.sh zlib`), smoke (`scripts/smoke-run.sh zlib EXTRA_LIBS="-lz"` → expect `zlib 1.3.1`), commit.

### Task 1.2: Port `libjpeg` (libjpeg-turbo, Wave 0)

**Files:** `ports/libjpeg/nxport.toml`, `smoke/libjpeg_smoke.c`

- [ ] **Step 1: Smoke test**

```c
#include <stdio.h>
#include <jpeglib.h>
int main(void){ struct jpeg_compress_struct c; struct jpeg_error_mgr e;
  c.err = jpeg_std_error(&e); jpeg_create_compress(&c);
  printf("libjpeg ok\n"); jpeg_destroy_compress(&c); return 0; }
```

- [ ] **Step 2: `nxport.toml`** — libjpeg-turbo is cmake:

```toml
name = "libjpeg"
source = "tar:https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.0.3/libjpeg-turbo-3.0.3.tar.gz"
build = "cmake"
configure = ["-DENABLE_SHARED=OFF", "-DENABLE_STATIC=ON", "-DWITH_SIMD=OFF",
             "-DCMAKE_INSTALL_PREFIX=/usr"]
install = "sysroot"
sysroot_libs = ["libjpeg.a", "libturbojpeg.a"]
sysroot_headers = ["jpeglib.h", "jconfig.h", "jmorecfg.h", "jerror.h", "turbojpeg.h"]
```

> `WITH_SIMD=OFF` because there is no NanOS assembler path for libjpeg-turbo's SIMD. The cmake
> cross-file is `nanos-sdk/port/toolchain-nanos.cmake` (driver supplies it; see writing-apps.md §4.6).

- [ ] **Step 3–5:** verify, smoke (`EXTRA_LIBS="-ljpeg"` → expect `libjpeg ok`), commit.

### Task 1.3: Port `expat` (Wave 0 — needed by libdom)

**Files:** `ports/expat/nxport.toml`, `ports/expat/cache.txt`, `smoke/expat_smoke.c`

- [ ] **Step 1: Smoke test**

```c
#include <stdio.h>
#include <expat.h>
int main(void){ XML_Parser p = XML_ParserCreate(NULL);
  printf("expat %s\n", XML_ExpatVersion()); XML_ParserFree(p); return 0; }
```

- [ ] **Step 2: `nxport.toml`** (autotools — copy the gnulib cache idiom from wget-port if configure stalls):

```toml
name = "expat"
source = "tar:https://github.com/libexpat/libexpat/releases/download/R_2_6_2/expat-2.6.2.tar.gz"
build = "autotools"
configure = ["--without-docbook", "--without-examples", "--without-tests",
             "--disable-shared", "--enable-static"]
cache = []  # fill from wget-port/cache if AC_CHECK_FUNC misdetects (honest-conftest should prevent it)
install = "sysroot"
sysroot_libs = ["libexpat.a"]
sysroot_headers = ["expat.h", "expat_external.h"]
```

- [ ] **Step 3–5:** verify, smoke (`EXTRA_LIBS="-lexpat"` → expect `expat 2.6.2`), commit.

### Task 1.4: Port `libpng` (Wave 1 — needs zlib in the sysroot)

**Files:** `ports/libpng/nxport.toml`, `smoke/libpng_smoke.c`

- [ ] **Step 1: Smoke test**

```c
#include <stdio.h>
#include <png.h>
int main(void){ printf("libpng %s\n", png_get_libpng_ver(NULL)); return 0; }
```

- [ ] **Step 2: `nxport.toml`**

```toml
name = "libpng"
source = "tar:https://download.sourceforge.net/libpng/libpng-1.6.43.tar.gz"
build = "autotools"
configure = ["--disable-shared", "--enable-static", "--with-zlib-prefix=/usr"]
cache = []  # libpng's configure is light; add forcings only if needed
install = "sysroot"
sysroot_libs = ["libpng16.a"]
sysroot_headers = ["png.h", "pngconf.h", "pnglibconf.h"]
```

> Verify ordering: zlib (Task 1.1) must be installed in the shared sysroot before the orchestrator
> builds libpng. The isolation verify (`port-verify.sh libpng`) seeds zlib into the temp sysroot by
> first running `port-verify.sh zlib` — or, simpler, run `scripts/build-all.sh --until libpng` once.

- [ ] **Step 3–5:** verify, smoke (`EXTRA_LIBS="-lpng16 -lz"` → expect `libpng 1.6.43`), commit.

### Task 1.5: Port `libcurl` (Wave 0/1 — needs OpenSSL; HTTPS backbone)

**Files:** `ports/libcurl/nxport.toml`, `ports/libcurl/cache.txt`, `smoke/libcurl_smoke.c`

- [ ] **Step 1: Smoke test**

```c
#include <stdio.h>
#include <curl/curl.h>
int main(void){ curl_global_init(CURL_GLOBAL_DEFAULT);
  printf("%s\n", curl_version()); curl_global_cleanup(); return 0; }
```

- [ ] **Step 2: `nxport.toml`** — the big one; disable everything NanOS lacks, keep OpenSSL:

```toml
name = "libcurl"
source = "tar:https://curl.se/download/curl-8.8.0.tar.gz"
build = "autotools"
configure = [
  "--disable-shared", "--enable-static",
  "--with-openssl=/usr",        # use the ported OpenSSL in the sysroot
  "--without-libpsl", "--without-zstd", "--without-brotli", "--without-libidn2",
  "--without-nghttp2", "--without-ngtcp2", "--without-librtmp",
  "--disable-ldap", "--disable-ldaps", "--disable-rtsp", "--disable-dict",
  "--disable-telnet", "--disable-tftp", "--disable-pop3", "--disable-imap",
  "--disable-smtp", "--disable-gopher", "--disable-mqtt", "--disable-smb",
  "--disable-ftp", "--disable-file",   # NetSurf only needs http/https
  "--disable-ipv6", "--disable-manual", "--disable-unix-sockets",
  "--disable-threaded-resolver",       # NO THREADS on NanOS — use the sync resolver
  "--enable-http", "--enable-proxy",
]
cache = []   # copy gnulib/socket forcings from wget-port/cache as needed
install = "sysroot"
sysroot_libs = ["libcurl.a"]
sysroot_headers = ["curl/curl.h", "curl/curlver.h", "curl/easy.h", "curl/multi.h",
                   "curl/system.h", "curl/urlapi.h", "curl/options.h",
                   "curl/header.h", "curl/websockets.h", "curl/mprintf.h"]
```

> **Critical:** `--disable-threaded-resolver`. curl's default async resolver spawns a thread; NanOS
> has none. The sync resolver uses NanOS's libc `getaddrinfo` (networking.md §6).

- [ ] **Step 3–7:** verify, **audit cache/config**, **run curl's test suite subset** (the protocol
      tests for http/https — host-built where possible), smoke
      (`EXTRA_LIBS="-lcurl -lssl -lcrypto -lz"` → expect a `libcurl/8.8.0 OpenSSL/...` line), commit.

### Task 1.6: Port `libiconv` (Wave 0 — charset conversion is foundational, NOT deferred)

**Files:** `ports/libiconv/nxport.toml`, `ports/libiconv/cache.txt`, `smoke/libiconv_smoke.c`

> A browser must convert real-world page encodings (UTF-8, the ISO-8859 family, Windows-125x, Shift-
> JIS, GBK, EUC, Big5, …). picolibc's `iconv` is incomplete, so we port **GNU libiconv** properly and
> wire libparserutils to use it (Task 2.2). This replaces the earlier "built-in subset, revisit later"
> shortcut — encoding correctness is foundational.

- [ ] **Step 1: Smoke test** `smoke/libiconv_smoke.c`

```c
#include <stdio.h>
#include <iconv.h>
int main(void){
    iconv_t cd = iconv_open("UTF-8", "ISO-8859-2");   /* Latin-2 -> UTF-8 */
    if (cd == (iconv_t)-1) { printf("iconv_open FAIL\n"); return 1; }
    char in[] = { (char)0xB3, 0 };                    /* 'ł' in ISO-8859-2 */
    char out[8] = {0}; char *ip = in, *op = out;
    size_t il = 1, ol = sizeof out;
    iconv(cd, &ip, &il, &op, &ol);
    iconv_close(cd);
    printf("iconv %02X%02X\n", (unsigned char)out[0], (unsigned char)out[1]); /* expect C5 82 (U+0142) */
    return 0;
}
```

- [ ] **Step 2: `nxport.toml`** (full encoding set — do not trim the charset list)

```toml
name = "libiconv"
source = "tar:https://ftp.gnu.org/pub/gnu/libiconv/libiconv-1.17.tar.gz"
build = "autotools"
configure = [
  "--disable-shared", "--enable-static",
  "--enable-extra-encodings",   # why: real pages use CJK/cyrillic/etc — keep the full set
]
cache = []   # derive + audit per the template (Step 4); GNU libiconv cross-compiles cleanly with honest-conftest
install = "sysroot"
sysroot_libs = ["libiconv.a", "libcharset.a"]
sysroot_headers = ["iconv.h", "libcharset.h", "localcharset.h"]
```

- [ ] **Step 3–7:** verify, audit cache/config, **run libiconv's `make check`** (its `tests/` cover
      round-trips across the encoding families — these MUST pass), smoke
      (`EXTRA_LIBS="-liconv -lcharset"` → expect `iconv C582`), commit.

---

## Phase 2 — NetSurf's own libraries

NetSurf libs build with the **NetSurf buildsystem** (not autotools). Task 2.0 sets it up once; the
rest follow a NetSurf-lib variant of the template (build = "make" with the NS env).

### Task 2.0: Vendor the NetSurf buildsystem + a NS-lib port helper

**Files:**
- Create: `ports/_nsbuild/nxport.toml`  (checks out `buildsystem` into the sysroot share dir)
- Create: `ports/_nsbuild/build-nslib.sh` (shared by every NetSurf-lib port hook)

- [ ] **Step 1: Write `ports/_nsbuild/build-nslib.sh`** — the cross invocation NetSurf libs use

```bash
#!/bin/sh
# Usage: build-nslib.sh <srcdir>
# NetSurf libs read NSSHARED for the shared buildsystem and honor CC/PREFIX/COMPONENT_TYPE.
set -e
SRC="$1"
export NSSHARED="${NSSHARED:-/usr/share/netsurf-buildsystem}"
export HOST=i686-nanos
export PREFIX=/usr
export COMPONENT_TYPE=lib-static
make -C "$SRC" \
  CC=i686-nanos-gcc AR=i686-nanos-ar \
  PREFIX=/usr DESTDIR="$DESTDIR" \
  Q= V=1 install
```

- [ ] **Step 2: Write `ports/_nsbuild/nxport.toml`** — installs the buildsystem into the sysroot share

```toml
name = "_nsbuild"
source = "git:https://git.netsurf-browser.org/buildsystem.git"
build = "make"
configure = ["PREFIX=/usr", "install"]
install = "sysroot"
sysroot_headers = []   # buildsystem is makefiles; the hook places it under /usr/share
```

> If `install=sysroot` doesn't place the makefiles where `NSSHARED` expects, add a `post_build.sh`
> hook that `cp -R` the buildsystem to `$STAGE_SYSROOT/usr/share/netsurf-buildsystem`.

- [ ] **Step 3: Commit** — `git add ports/_nsbuild && git commit -m "netsurf buildsystem vendor + ns-lib helper"`.

### Task 2.1: Port `libwapcaplet` (Wave 0)

**Files:** `ports/libwapcaplet/nxport.toml`, `ports/libwapcaplet/hooks/post_configure.sh`, `smoke/libwapcaplet_smoke.c`

- [ ] **Step 1: Smoke test**

```c
#include <stdio.h>
#include <libwapcaplet/libwapcaplet.h>
int main(void){ lwc_string *s; lwc_intern_string("html", 4, &s);
  printf("lwc %zu\n", lwc_string_length(s)); lwc_string_unref(s); return 0; }
```

- [ ] **Step 2: `nxport.toml`** (NetSurf-lib pattern: build="make", hook calls the shared helper)

```toml
name = "libwapcaplet"
source = "git:https://git.netsurf-browser.org/libwapcaplet.git"
build = "make"
configure = []           # the post_configure hook drives the real build
install = "sysroot"
sysroot_libs = ["libwapcaplet.a"]
sysroot_headers = ["libwapcaplet/libwapcaplet.h"]
```

- [ ] **Step 3: `hooks/post_configure.sh`**

```bash
#!/bin/sh
set -e
. /work/port/../_nsbuild/build-nslib.sh "$STAGE"
```

- [ ] **Step 4–6:** verify, smoke (`EXTRA_LIBS="-lwapcaplet"` → expect `lwc 4`), commit.

### Task 2.2: Port `libparserutils` (Wave 0)

**Files:** `ports/libparserutils/nxport.toml`, hook, `smoke/libparserutils_smoke.c`

- [ ] **Step 1: Smoke test**

```c
#include <stdio.h>
#include <parserutils/parserutils.h>
int main(void){ printf("parserutils %d\n", (int)PARSERUTILS_OK); return 0; }
```

- [ ] **Step 2: `nxport.toml`** — wire the **full iconv filter** (Task 1.6 libiconv), NOT the
      built-in subset. Charset coverage is foundational; this is done right here.

```toml
name = "libparserutils"
source = "git:https://git.netsurf-browser.org/libparserutils.git"
build = "make"
configure = []
needs = []                       # libiconv is a static sysroot lib, linked by the consumer (netsurf app)
install = "sysroot"
sysroot_libs = ["libparserutils.a"]
sysroot_headers = ["parserutils/parserutils.h", "parserutils/charset/mibenum.h",
                   "parserutils/input/inputstream.h"]
```

> The NS build var selecting the charset backend MUST be set to **iconv** (NOT the built-in subset)
> in this lib's hook — pass the iconv-filter option + point it at the ported libiconv
> (`-I$PREFIX/include -L$PREFIX/lib -liconv`). Confirm the exact var name in
> `libparserutils/Makefile`/`Makefile.defaults` (it gates `src/charset/codecs/codec_iconv.c`); the
> goal is that `codec_iconv` is compiled in, so every encoding libiconv supports is available.

- [ ] **Step 3: hook** (same as 2.1, plus the iconv-filter env override)
- [ ] **Step 4–7:** verify, audit config (confirm `codec_iconv` was built, not the 8859 stub),
      **run libparserutils' `test/` suite** (it has charset round-trip + inputstream tests — these
      exercise exactly the part we must not shortcut), smoke
      (`EXTRA_LIBS="-lparserutils -liconv -lcharset"` → expect `parserutils 0`), commit.

### Task 2.3: Ports `libnsutils`, `libnslog`, `libnsgif`, `libnsbmp` (Wave 0 — four tiny parallel tasks)

> Four independent tasks; one agent each, or one agent in sequence. All are pure-C, no NetSurf deps.
> Each follows Task 2.1's pattern (NS-lib `nxport.toml` + shared hook + smoke). Specifics:

- [ ] **`libnsutils`** — `source = git:.../libnsutils.git`; smoke includes `<nsutils/time.h>`, calls
      `nsu_getmonotonic_ms(&ms)`, prints it. `sysroot_headers = ["nsutils/*"]`.
- [ ] **`libnslog`** — `source = git:.../libnslog.git`; smoke calls `nslog_init(NULL,NULL,0)` prints `nslog ok`.
- [ ] **`libnsgif`** — `source = git:.../libnsgif.git`; smoke calls `nsgif_create(...)`/`nsgif_destroy`, prints `nsgif ok`.
- [ ] **`libnsbmp`** — `source = git:.../libnsbmp.git`; smoke calls `bmp_create(...)`/`bmp_finalise`, prints `nsbmp ok`.
- [ ] **Commit each separately** (`port: libnsXXX`).

### Task 2.4: Port `libnsfb` (core, Wave 0) — WITHOUT the NanWM backend yet

**Files:** `ports/libnsfb/nxport.toml`, hook, `smoke/libnsfb_smoke.c`

- [ ] **Step 1: Smoke test** (RAM surface — no display needed)

```c
#include <stdio.h>
#include <libnsfb.h>
int main(void){ nsfb_t *fb = nsfb_new(NSFB_SURFACE_RAM);
  printf("libnsfb %s\n", fb ? "ok" : "null"); return fb ? 0 : 1; }
```

- [ ] **Step 2: `nxport.toml`** — build with the RAM + (host) backends only for now; the NanWM
      backend is added in Phase 3 and rebuilt.

```toml
name = "libnsfb"
source = "git:https://git.netsurf-browser.org/libnsfb.git"
build = "make"
configure = []
install = "sysroot"
sysroot_libs = ["libnsfb.a"]
sysroot_headers = ["libnsfb.h", "libnsfb_plot.h", "libnsfb_plot_util.h",
                   "libnsfb_event.h", "libnsfb_cursor.h"]
```

- [ ] **Step 3–6:** hook, verify, smoke (`EXTRA_LIBS="-lnsfb"` → expect `libnsfb ok`), commit.

### Task 2.5: Port `libhubbub` (Wave 1 — needs libparserutils + libwapcaplet)

**Files:** `ports/libhubbub/nxport.toml`, hook, `smoke/libhubbub_smoke.c`

- [ ] **Step 1: Smoke test**

```c
#include <stdio.h>
#include <hubbub/hubbub.h>
int main(void){ printf("hubbub %d\n", (int)HUBBUB_OK); return 0; }
```

- [ ] **Step 2: `nxport.toml`** (NS-lib; deps already in sysroot via orchestrator order)

```toml
name = "libhubbub"
source = "git:https://git.netsurf-browser.org/libhubbub.git"
build = "make"
configure = []
needs = []
install = "sysroot"
sysroot_libs = ["libhubbub.a"]
sysroot_headers = ["hubbub/hubbub.h", "hubbub/parser.h", "hubbub/tree.h",
                   "hubbub/types.h", "hubbub/errors.h", "hubbub/functypes.h"]
```

- [ ] **Step 3–6:** hook, verify (seed parserutils+wapcaplet into the temp sysroot first — run
      `scripts/build-all.sh --until libhubbub`), smoke (`EXTRA_LIBS="-lhubbub -lparserutils -lwapcaplet"`
      → expect `hubbub 0`), commit.

### Task 2.6: Port `libcss` (Wave 1 — needs libparserutils + libwapcaplet)

**Files:** `ports/libcss/nxport.toml`, hook, `smoke/libcss_smoke.c`

- [ ] **Step 1: Smoke test**

```c
#include <stdio.h>
#include <libcss/libcss.h>
int main(void){ printf("libcss %d\n", (int)CSS_OK); return 0; }
```

- [ ] **Step 2: `nxport.toml`**

```toml
name = "libcss"
source = "git:https://git.netsurf-browser.org/libcss.git"
build = "make"
configure = []
install = "sysroot"
sysroot_libs = ["libcss.a"]
sysroot_headers = ["libcss/libcss.h", "libcss/computed.h", "libcss/errors.h",
                   "libcss/types.h", "libcss/properties.h", "libcss/select.h",
                   "libcss/stylesheet.h", "libcss/hint.h", "libcss/fpmath.h",
                   "libcss/font_face.h", "libcss/unit.h"]
```

> libcss build runs `gperf` and a perl property generator on the **host** — ensure `gperf` + `perl`
> are in the `nanos-sdk-dev` image. If absent, add them in a `pre_configure.sh` (apt) or extend the
> image (Task 2.0 note).

- [ ] **Step 3–6:** hook, verify, smoke (`EXTRA_LIBS="-lcss -lparserutils -lwapcaplet"` → `libcss 0`), commit.

### Task 2.7: Port `libdom` (Wave 2 — needs libhubbub + libparserutils + libwapcaplet + expat)

**Files:** `ports/libdom/nxport.toml`, hook, `smoke/libdom_smoke.c`

- [ ] **Step 1: Smoke test**

```c
#include <stdio.h>
#include <dom/dom.h>
int main(void){ printf("libdom %d\n", (int)DOM_NO_ERR); return 0; }
```

- [ ] **Step 2: `nxport.toml`**

```toml
name = "libdom"
source = "git:https://git.netsurf-browser.org/libdom.git"
build = "make"
configure = []
install = "sysroot"
sysroot_libs = ["libdom.a"]
sysroot_headers = ["dom/dom.h", "dom/core.h", "dom/events.h", "dom/html.h",
                   "dom/bootstrap/init_fini.h", "dom/bootstrap/implpriv.h"]
```

> libdom's HTML path uses libhubbub; its XML path uses expat — both must be in the sysroot. The NS
> build var `WITH_LIBXML_BINDING=no WITH_EXPAT_BINDING=yes` selects expat for XML. Set it in the hook.

- [ ] **Step 3–6:** hook, verify (`scripts/build-all.sh --until libdom`), smoke
      (`EXTRA_LIBS="-ldom -lhubbub -lparserutils -lwapcaplet -lexpat"` → `libdom 0`), commit.

---

## Phase 3 — The NanWM frontend (the bespoke integration)

This is the heart of the port and the one piece that is **new code**, not a port. It is a new
`libnsfb` surface backend that maps a NetSurf framebuffer onto a NanWM window. Develop it against
the installed `libnsfb` + the NanOS `libnw` headers.

### Task 3.1: Add a NanWM surface backend to libnsfb

**Files:**
- Create: `frontend-nanwm/nsfb_nanwm.c`        (the backend)
- Create: `frontend-nanwm/register.patch`      (adds NSFB_SURFACE_NANWM to libnsfb's enum + registration)
- Create: `frontend-nanwm/keymap.h`            (NanWM scancode/ASCII → nsfb keycode)
- Modify: `ports/libnsfb/nxport.toml`          (add a pre_configure hook that injects our backend)
- Create: `ports/libnsfb/hooks/pre_configure.sh`
- Create: `smoke/nsfb_nanwm_smoke.c`

- [ ] **Step 1: Study the templates** — read libnsfb `src/surface/linux.c` (fbdev: shows the
      surface_rtns shape — `initialise/finalise/input/claim/update/cursor/geometry/parameters` — and
      how a backend fills `nsfb->ptr/linelen/bpp/format/width/height`) and `src/surface/sdl.c` (shows
      event translation into `nsfb_event_t`). Our backend mirrors `linux.c`'s structure but the
      "device" is a NanWM window, and mirrors `sdl.c`'s event loop but the source is `nw_next_event`.

- [ ] **Step 2: Write `frontend-nanwm/keymap.h`** — translate a NanWM key event to an nsfb keycode

```c
#ifndef NSFB_NANWM_KEYMAP_H
#define NSFB_NANWM_KEYMAP_H
#include <libnsfb_event.h>
#include <libnw.h>
/* NanWM delivers ASCII in ev.ch (0 if none) + raw scancode in ev.code. NetSurf's framebuffer
 * frontend mostly wants ASCII + a handful of control keys. Map printable ASCII straight through;
 * map the few specials NetSurf navigation needs. Returns an NSFB_KEY_* code (0 = drop). */
static inline enum nsfb_key_code_e nanwm_map_key(const struct nw_event *ev)
{
    if (ev->ch >= 0x20 && ev->ch < 0x7f) return (enum nsfb_key_code_e)ev->ch; /* ASCII == NSFB_KEY_<ascii> */
    switch (ev->ch) {
        case '\n': case '\r': return NSFB_KEY_RETURN;
        case '\b':            return NSFB_KEY_BACKSPACE;
        case 0x1b:            return NSFB_KEY_ESCAPE;
        case '\t':            return NSFB_KEY_TAB;
    }
    /* Arrows/page keys arrive as ANSI sequences in cooked mode; in NanWM they come as scancodes.
     * Fill these from arch/x86 set-1 scancodes (see kernel/KeyDecoder) as navigation needs them. */
    switch (ev->code) {
        case 0x48: return NSFB_KEY_UP;
        case 0x50: return NSFB_KEY_DOWN;
        case 0x4b: return NSFB_KEY_LEFT;
        case 0x4d: return NSFB_KEY_RIGHT;
        case 0x49: return NSFB_KEY_PAGEUP;
        case 0x51: return NSFB_KEY_PAGEDOWN;
        default:   return NSFB_KEY_NONE;
    }
}
#endif
```

- [ ] **Step 3: Write `frontend-nanwm/nsfb_nanwm.c`** — the backend (skeleton; mirror `linux.c`)

```c
/* nsfb_nanwm.c — libnsfb surface backend drawing into a NanWM window via libnw.
 * Registered as NSFB_SURFACE_NANWM. Mirrors src/surface/linux.c (structure) and sdl.c (events). */
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include "libnsfb.h"
#include "libnsfb_event.h"
#include "libnsfb_plot.h"
#include "nsfb.h"          /* internal: struct nsfb_s */
#include "surface.h"       /* internal: nsfb_surface_rtns, _nsfb_register_surface */
#include "plot.h"
#include "cursor.h"
#include <libnw.h>
#include "keymap.h"

struct nanwm_priv { nw_display *d; nw_win *win; };

static int nanwm_set_geometry(nsfb_t *nsfb, int w, int h, enum nsfb_format_e fmt)
{
    /* NanWM windows are server-sized; we accept the requested size and let CONFIGURE confirm. */
    if (nsfb->surface_priv != NULL) return -1; /* can't change after init in this simple backend */
    nsfb->width = w; nsfb->height = h;
    /* nw_surface is 0x00RRGGBB uint32 LE (BGRX in memory). Pick the matching nsfb format and
     * verify empirically in the smoke test (Step 7) that a red fill is red, not blue. */
    nsfb->format = NSFB_FMT_XRGB8888;
    select_plotters(nsfb);  /* libnsfb picks the right plot routines for the format */
    return 0;
}

static int nanwm_initialise(nsfb_t *nsfb)
{
    struct nanwm_priv *p = calloc(1, sizeof *p);
    if (!p) return -1;
    p->d = nw_connect();
    if (!p->d) { free(p); return -1; }
    p->win = nw_create_window(p->d, nsfb->width, nsfb->height, "NetSurf");
    if (!p->win) { free(p); return -1; }
    struct nw_surface s; nw_win_surface(p->win, &s);
    nsfb->ptr     = (uint8_t *)s.px;
    nsfb->linelen = s.stride * 4;        /* bytes per row */
    nsfb->bpp     = 32;
    nsfb->width   = s.w;
    nsfb->height  = s.h;
    nsfb->surface_priv = p;
    return 0;
}

static int nanwm_finalise(nsfb_t *nsfb)
{
    struct nanwm_priv *p = nsfb->surface_priv;
    if (p) { /* libnw has no explicit destroy; closing fds 3/4 ends the client */ free(p); }
    nsfb->surface_priv = NULL;
    return 0;
}

static bool nanwm_input(nsfb_t *nsfb, nsfb_event_t *event, int timeout)
{
    struct nanwm_priv *p = nsfb->surface_priv;
    struct nw_event ev;
    int r = nw_next_event(p->d, &ev, timeout);
    if (r != 1) return false;                 /* 0 = timeout, -1 = closed */
    switch (ev.type) {
    case NW_EV_KEY: {
        enum nsfb_key_code_e k = nanwm_map_key(&ev);
        if (k == NSFB_KEY_NONE) return false;
        event->type = ev.down ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP;
        event->value.keycode = k;
        return true;
    }
    case NW_EV_POINTER:
        /* NetSurf wants absolute moves + button transitions. Emit a move; buttons handled below. */
        event->type = NSFB_EVENT_MOVE_ABSOLUTE;
        event->value.vector.x = ev.x;
        event->value.vector.y = ev.y;
        event->value.vector.z = 0;
        /* NOTE: NanWM gives a button bitmask, not edges. Track previous mask in nanwm_priv and emit
         * NSFB_EVENT_KEY_DOWN/UP with NSFB_KEY_MOUSE_1.. on change in a follow-up if clicks lag. */
        return true;
    case NW_EV_CLOSE:
        event->type = NSFB_EVENT_CONTROL;
        event->value.controlcode = NSFB_CONTROL_QUIT;
        return true;
    default:
        return false;
    }
}

static int nanwm_claim(nsfb_t *nsfb, nsfb_bbox_t *box) { (void)nsfb; (void)box; return 0; }

static int nanwm_update(nsfb_t *nsfb, nsfb_bbox_t *box)
{
    struct nanwm_priv *p = nsfb->surface_priv;
    nw_commit(p->win, box->x0, box->y0, box->x1 - box->x0, box->y1 - box->y0);
    return 0;
}

static int nanwm_cursor(nsfb_t *nsfb, struct nsfb_cursor_s *cursor)
{
    (void)nsfb; (void)cursor;            /* NanWM draws its own cursor; nothing to do */
    return true;
}

const nsfb_surface_rtns_t nanwm_rtns = {
    .initialise = nanwm_initialise,
    .finalise   = nanwm_finalise,
    .input      = nanwm_input,
    .claim      = nanwm_claim,
    .update     = nanwm_update,
    .cursor     = nanwm_cursor,
    .geometry   = nanwm_set_geometry,
};

NSFB_SURFACE_DEF(nanwm, NSFB_SURFACE_NANWM, &nanwm_rtns)
```

> The exact internal symbol names (`select_plotters`, `NSFB_SURFACE_DEF`, `nsfb->surface_priv`, the
> `nsfb_surface_rtns_t` field set, `NSFB_FMT_*`, `NSFB_KEY_*`, `NSFB_EVENT_*`, `NSFB_CONTROL_QUIT`)
> MUST be confirmed against the libnsfb checkout — copy them verbatim from `src/surface/linux.c` and
> `src/surface/sdl.c`. The skeleton above is structurally faithful to those files; adjust names to
> match the exact version pulled.

- [ ] **Step 4: Write `frontend-nanwm/register.patch`** — add the enum value + register the backend.
      libnsfb registers backends via `NSFB_SURFACE_DEF(...)` (a constructor/section) and an enum in
      `include/libnsfb.h` (`enum nsfb_type_e`). Add `NSFB_SURFACE_NANWM,` to that enum and add
      `nsfb_nanwm.c` to `src/surface/Makefile`. Capture both as a unified diff:

```diff
--- a/include/libnsfb.h
+++ b/include/libnsfb.h
@@ enum nsfb_type_e {
     NSFB_SURFACE_RAM,
+    NSFB_SURFACE_NANWM,
     NSFB_SURFACE_SDL,
--- a/src/surface/Makefile
+++ b/src/surface/Makefile
@@
 DIR_SOURCES := ... sdl.c linux.c
+DIR_SOURCES += nsfb_nanwm.c
```

> Confirm the real enum block + Makefile var name in the pulled libnsfb and regenerate this diff so
> it applies cleanly (`patch -p1 --dry-run`).

- [ ] **Step 5: Write `ports/libnsfb/hooks/pre_configure.sh`** — inject our backend before build

```bash
#!/bin/sh
set -e
# $STAGE = libnsfb source tree (set by nanos-port)
cp /work/port/../../frontend-nanwm/nsfb_nanwm.c "$STAGE/src/surface/nsfb_nanwm.c"
cp /work/port/../../frontend-nanwm/keymap.h     "$STAGE/src/surface/keymap.h"
# libnw headers come from the NanOS checkout; make them visible to the build
cp "$NANOS"/user/libnw/libnw.h   "$STAGE/include/" 2>/dev/null || true
cp "$NANOS"/user/libnw/nw_gfx.h  "$STAGE/include/" 2>/dev/null || true
patch -p1 -d "$STAGE" < /work/port/../../frontend-nanwm/register.patch
```

> Add `--need libnw.ndl` is NOT here (this is a static lib); the **app** (Phase 4) links libnw. But
> the backend's `#include <libnw.h>` must resolve at libnsfb compile time — hence the header copy.

- [ ] **Step 6: Update `ports/libnsfb/nxport.toml`** — register the hook + the libnw build dep

```toml
# (add to the existing libnsfb nxport.toml from Task 2.4)
# pre_configure.sh injects nsfb_nanwm.c + keymap.h + the register patch + libnw headers.
```

- [ ] **Step 7: Smoke test** `smoke/nsfb_nanwm_smoke.c` — open a NanWM window via libnsfb, fill it
      red, commit, wait for close. Run it **under `nwm`** (it's a GUI client) and eyeball the colour
      (this is also the BGRX/format verification — if the window is blue, switch
      `NSFB_FMT_XRGB8888`→`NSFB_FMT_XBGR8888` in `nanwm_set_geometry` and rebuild).

```c
#include <libnsfb.h>
#include <libnsfb_plot.h>
#include <libnsfb_event.h>
int main(void){
    nsfb_t *fb = nsfb_new(NSFB_SURFACE_NANWM);
    if (!fb) return 1;
    nsfb_set_geometry(fb, 640, 480, NSFB_FMT_ANY);
    if (nsfb_init(fb) != 0) return 2;
    nsfb_bbox_t box = {0,0,640,480};
    uint8_t *buf; int len; nsfb_get_buffer(fb, &buf, &len);
    nsfb_plot_rectangle_fill(fb, &box, 0xffff0000);   /* opaque red */
    nsfb_update(fb, &box);
    nsfb_event_t e; while (nsfb_event(fb, &e, -1)) { if (e.type==NSFB_EVENT_CONTROL) break; }
    return 0;
}
```

- [ ] **Step 8: Build + run under nwm** (manually for now: `make image`, boot, run `nwm`, then run
      `nsfb_nanwm_smoke`). Expected: a red 640×480 window in NanWM; closing it exits cleanly.

- [ ] **Step 9: Commit** — `git add frontend-nanwm ports/libnsfb smoke/nsfb_nanwm_smoke.c && git commit -m "frontend: NanWM libnsfb backend"`.

---

## Phase 4 — Build the NetSurf app (no JS) + run it

### Task 4.1: Port the NetSurf framebuffer frontend → `netsurf.nxe`

**Files:**
- Create: `ports/netsurf/nxport.toml`
- Create: `ports/netsurf/hooks/pre_configure.sh`
- Create: `ports/netsurf/Makefile.config`  (NetSurf build options: pick fb frontend, disable extras)

- [ ] **Step 1: Write `ports/netsurf/Makefile.config`** — NetSurf's option file selecting the
      framebuffer frontend, the NanWM surface, internal bitmap fonts (no FreeType on first cut),
      curl fetcher, and disabling JS for now.

```make
# NetSurf build options (framebuffer frontend, NanWM surface, no JS yet)
NETSURF_USE_CURL := YES
NETSURF_USE_OPENSSL := YES
NETSURF_USE_PNG := YES
NETSURF_USE_JPEG := YES
NETSURF_USE_NSGIF := YES
NETSURF_USE_NSBMP := YES
NETSURF_USE_DUKTAPE := NO          # JS off for the first working build (Phase 5 flips this)
NETSURF_USE_FREETYPE := NO         # use the built-in framebuffer bitmap fonts first
NETSURF_FB_FONTLIB := internal
NETSURF_FB_FRONTEND := nanwm       # our libnsfb surface backend (NSFB_SURFACE_NANWM)
NETSURF_USE_HARU_PDF := NO
NETSURF_USE_WEBP := NO
NETSURF_USE_VIDEO := NO
```

> The exact var spelling lives in `frontends/framebuffer/Makefile.defaults`. Confirm
> `NETSURF_FB_FRONTEND`/surface selection — the framebuffer frontend chooses the libnsfb surface at
> runtime via `fb_surface` config or `-f` flag; if it's runtime not compile-time, pass `-f nanwm`
> on the command line instead and drop `NETSURF_FB_FRONTEND` here.

- [ ] **Step 2: Write `ports/netsurf/hooks/pre_configure.sh`** — drop in the option file + libnw lib

```bash
#!/bin/sh
set -e
cp /work/port/Makefile.config "$STAGE/frontends/framebuffer/Makefile.config"
# the app links libnw.ndl — make its import lib + .ndl available to the linker/mknx
cp "$NANOS"/bin/libnw.ndl.a "$SDK_TC/i686-nanos/lib/" 2>/dev/null || true
cp "$NANOS"/bin/libnw.ndl   "$SDK_TC/i686-nanos/lib/" 2>/dev/null || true
```

- [ ] **Step 3: Write `ports/netsurf/nxport.toml`**

```toml
name = "netsurf"
source = "git:https://git.netsurf-browser.org/netsurf.git"
build = "make"
configure = ["TARGET=framebuffer", "Q=", "V=1"]
needs = ["libc.ndl", "libnw.ndl"]      # mknx records the .ndl deps; libnsfb etc. are static
binary = "nsfb"                        # NetSurf fb binary name (confirm: 'nsfb' under build dir)
install = "/apps/netsurf"
data = [
  "frontends/framebuffer/res -> /apps/netsurf/res",   # fonts, default CSS, icons NetSurf needs
]
```

> NetSurf's fb binary is conventionally `nsfb`; the build dir is `build-framebuffer-framebuffer/`.
> Set `binary` to the real path after the first build (the driver reports where the ELF landed).
> `--need libnw.ndl` is emitted by `needs`; the static NetSurf/dep libs are pulled from the sysroot
> by `-l` flags the NetSurf Makefile already passes.

- [ ] **Step 4: Build (orchestrated, full ordered sysroot)** — everything before netsurf must be
      installed first:

```bash
cd ~/Projects/netsurf-nanos
scripts/build-all.sh        # builds all ports in dependency order into the shared sysroot, then netsurf
```

Expected: `ports/netsurf/netsurf.nxe` produced. Iterate on missing symbols / headers by adjusting
the relevant lib's `sysroot_headers` and re-running. (This is where most debugging time goes.)

- [ ] **Step 5: Commit** — `git add ports/netsurf && git commit -m "port: netsurf framebuffer frontend (no JS)"`.

### Task 4.2: First render — load a local page, then a real site

**Files:** none new (verification task)

- [ ] **Step 1: Install + boot** — copy `netsurf.nxe` + `res/` into the NanOS image (Phase 5
      automates this; for now use the manual `debugfs write` recipe), boot, run `nwm`.
- [ ] **Step 2: Render a local file** — from the shell inside nwm, run NetSurf on a `file://` URL
      pointing at a tiny test page shipped in `/apps/netsurf/res/test.html`. Expected: a window with
      laid-out text + a coloured box (proves HTML+CSS+paint+commit end to end, no network).
- [ ] **Step 3: Render over HTTP** — `netsurf http://example.com` (DNS → TCP → HTTP → render).
      Expected: the example.com page renders with its heading + paragraph.
- [ ] **Step 4: Render over HTTPS** — `netsurf https://example.com` (needs `make openssl` done +
      `/nanos/ssl/cert.pem` present). Expected: the page renders; no TLS verify error.
- [ ] **Step 5: Screendump evidence** — capture PNGs of steps 2–4 via the headless QEMU screendump
      recipe (writing-apps.md §9) and save under `netsurf-nanos/docs/shots/`. Commit them.

---

## Phase 5 — NanOS-side integration (image install + repeatable target)

### Task 5.0: NanOS smoke helper (referenced by Phase 0 Task 0.3)

**Files:**
- Create: `~/Projects/NanOS/scripts/run-smoke-nxe.sh`

- [ ] **Step 1: Write the helper** — install one `.nxe` into a copy of the image, boot headless, run
      it from the shell, screendump stdout. Model it on the existing headless-QEMU recipe in
      `docs/en/writing-apps.md` §9 and the existing `scripts/*-qemu.sh` helpers.

```bash
#!/bin/sh
# Usage: run-smoke-nxe.sh <local.nxe> <image-path>
set -e
NXE="$1"; DST="$2"
cd "$(dirname "$0")/.."
cp disk/image-grub2.img /tmp/smoke.img
PART=/tmp/smoke.img   # offset-handling as in scripts/create-grub2-image.sh consumers
printf "mkdir /apps/smoke\nrm %s\nwrite %s %s\n" "$DST" "$NXE" "$DST" | debugfs -w "$PART" 2>/dev/null || true
# boot headless, autologin shell runs the nxe; screendump after a fixed delay (reuse openssl-smoke-qemu.sh pattern)
sh scripts/openssl-smoke-qemu.sh /tmp/smoke.img "$DST" || true
```

> Reuse the proven monitor/screendump flow already in `scripts/openssl-smoke-qemu.sh` (present in the
> repo) rather than re-inventing it; parameterize it for an arbitrary `.nxe` path.

- [ ] **Step 2: Commit (NanOS repo)** — `git add scripts/run-smoke-nxe.sh && git commit -m "scripts: run a single .nxe under headless QEMU (smoke)"`.

### Task 5.1: `make netsurf` target in NanOS

**Files:**
- Modify: `~/Projects/NanOS/Makefile`  (add a `netsurf:` target next to `openssl:`/`wget:`)

- [ ] **Step 1: Add the target** — mirror the `openssl:` target (Makefile ~line 169): sync the
      sysroot, build the whole `netsurf-nanos` repo via its orchestrator in the `nanos-sdk-dev`
      container, copy `netsurf.nxe` + `res/` into `bin/` staging.

```makefile
NETSURF_REPO ?= $(HOME)/Projects/netsurf-nanos
netsurf: bin/libc.ndl bin/libc.ndl.a bin/libnw.ndl bin/libnw.ndl.a
	@test -d "$(SDK_TC)/i686-nanos/include" || { echo "nanos-sdk toolchain not found"; exit 1; }
	@test -f "$(BINFOLDER)openssl.nxe" || echo "warn: openssl not built — HTTPS will be unavailable"
	sh $(NETSURF_REPO)/scripts/sync-sysroot.sh
	docker run --rm \
	  -v "$(SDK_TC)":/work/toolchain -v "$(NETSURF_REPO)":/work/repo -v "$(NANOS_SDK)":/sdk \
	  -e SDK=/sdk -e NANOS=/work/nanos -v "$(CURDIR)":/work/nanos \
	  -e PATH="/work/toolchain/bin:/usr/bin:/bin" \
	  -w /work/repo nanos-sdk-dev:latest sh scripts/build-all.sh
	cp "$(NETSURF_REPO)/ports/netsurf/netsurf.nxe" $(BINFOLDER)netsurf.nxe
	rm -rf $(BINFOLDER)netsurf-res && cp -R "$(NETSURF_REPO)/ports/netsurf/res" $(BINFOLDER)netsurf-res
	@echo "staged netsurf.nxe — run 'make image' to install it into /apps/netsurf"
```

- [ ] **Step 2: Commit** — `git commit -am "build: make netsurf (builds the netsurf-nanos port)"`.

### Task 5.2: Install NetSurf into the disk image

**Files:**
- Modify: `~/Projects/NanOS/Makefile`  (the `_image` target — add the netsurf bundle, like the openssl block ~line 488)

- [ ] **Step 1: Add the install block** to `_image` (guarded — skip if absent, like openssl)

```makefile
	# netsurf (optional, external): graphical browser bundle in /apps/netsurf + /bin symlink.
	if [ -f $(BINFOLDER)netsurf.nxe ]; then \
	  printf "mkdir /apps/netsurf\n" | debugfs -w "$(IMAGE_GRUB2_PART)" 2>/dev/null; \
	  printf "rm /apps/netsurf/netsurf.nxe\nwrite $(BINFOLDER)netsurf.nxe /apps/netsurf/netsurf.nxe\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
	  debugfs -w "$(IMAGE_GRUB2_PART)" -R "rm /bin/netsurf.nxe" 2>/dev/null; \
	  debugfs -w "$(IMAGE_GRUB2_PART)" -R "symlink /bin/netsurf.nxe /apps/netsurf/netsurf.nxe"; \
	  for f in $(BINFOLDER)netsurf-res/*; do \
	    bn=$$(basename $$f); \
	    printf "rm /apps/netsurf/$$bn\nwrite $$f /apps/netsurf/$$bn\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
	  done; \
	fi
```

> Mirrors the existing openssl install block (writes a `.nxe`, here also a `/bin` symlink + the `res/`
> data — the app-bundle pattern from filesystem.md §2). The `res/` copy is shallow; if NetSurf's
> resource tree is nested, replace the `for` loop with `rsync`-style recursive `debugfs` writes or a
> tarball-unpack step.

- [ ] **Step 2: Build + verify** — `make netsurf && make image && make run`; in nwm run `netsurf`
      (resolved by name via the `/bin` link farm). Expected: the browser launches.
- [ ] **Step 3: Commit** — `git commit -am "image: install netsurf bundle into /apps/netsurf + /bin"`.

### Task 5.3: Optionally auto-launch from the desktop

**Files:**
- Modify: `~/Projects/NanOS/user/nwm/nwm.c`  (spawn list, like `terminal`/`settings`/`nwexp`)

- [ ] **Step 1:** add `spawn_client(slot, "/disks/main/apps/netsurf/netsurf.nxe")` to the boot spawn
      list (only if you want NetSurf to open with the desktop — otherwise launch via Super+R). Keep
      this OFF by default to avoid slowing desktop boot; document the one-line toggle.
- [ ] **Step 2: Commit** — `git commit -am "nwm: optional netsurf auto-launch (off by default)"`.

---

## Phase 6 — JavaScript (Duktape) — OPTIONAL, after a working browser

### Task 6.1: Port `duktape`

**Files:** `ports/duktape/nxport.toml`, `smoke/duktape_smoke.c`

- [ ] **Step 1: Smoke test**

```c
#include <stdio.h>
#include "duktape.h"
int main(void){ duk_context *c = duk_create_heap_default();
  duk_eval_string(c, "1+2"); printf("duk %d\n", (int)duk_get_int(c,-1));
  duk_destroy_heap(c); return 0; }
```

- [ ] **Step 2: `nxport.toml`** — duktape ships a prepared single-source dist; build with plain make

```toml
name = "duktape"
source = "tar:https://duktape.org/duktape-2.7.0.tar.xz"
build = "make"
configure = ["-f", "Makefile.cmdline"]   # or compile src/duktape.c directly via a hook
install = "sysroot"
sysroot_libs = ["libduktape.a"]          # a post_build hook archives duktape.o into libduktape.a
sysroot_headers = ["duktape.h", "duk_config.h"]
```

> Simplest: a `post_build.sh` that does `i686-nanos-gcc -O2 -c src/duktape.c -o duktape.o &&
> i686-nanos-ar rcs libduktape.a duktape.o` and copies the two headers. Duktape is single-threaded,
> pure C — no NanOS-specific config needed beyond `duk_config.h` defaults.

- [ ] **Step 3–5:** verify, smoke (`EXTRA_LIBS="-lduktape -lm"` → expect `duk 3`), commit.

### Task 6.2: Ensure `nsgenbind` is available (host build tool)

**Files:** `ports/nsgenbind/nxport.toml` (host tool, install into the container)

- [ ] **Step 1:** `nsgenbind` is a **host** tool that generates NetSurf's JS↔DOM bindings at build
      time. Build it for the *host* (not i686-nanos) inside `nanos-sdk-dev`:

```toml
name = "nsgenbind"
source = "git:https://git.netsurf-browser.org/nsgenbind.git"
build = "make"
configure = ["PREFIX=/usr", "install"]   # host CC, installs into the container image
install = "sysroot"                      # placed where the netsurf build's BUILD_CC can find it
```

> If `nsgenbind` isn't on PATH at NetSurf build time, libdom JS binding generation fails. Confirm
> `nsgenbind --version` runs in the container before Task 6.3.

- [ ] **Step 2: Commit.**

### Task 6.3: Rebuild NetSurf with JS enabled

**Files:**
- Modify: `ports/netsurf/Makefile.config`  (flip the JS flags)

- [ ] **Step 1:** set `NETSURF_USE_DUKTAPE := YES` and `NETSURF_USE_JS := YES` in
      `ports/netsurf/Makefile.config`. The NetSurf build will run `nsgenbind` to generate the DOM
      bindings and link Duktape.
- [ ] **Step 2: Rebuild** — `scripts/build-all.sh` (or `make netsurf` from NanOS).
- [ ] **Step 3: Verify** — render a page with a trivial inline script
      (`<script>document.body.innerHTML='JS works'</script>`) served from `/apps/netsurf/res/js.html`.
      Expected: the page shows "JS works" (proves the engine + DOM bindings are live).
- [ ] **Step 4: Commit** — `git commit -am "netsurf: enable Duktape JS"`.

> **Expectation gate:** NetSurf's DOM binding coverage is partial/ES5. Simple scripts and basic DOM
> manipulation work; modern frameworks (React/Vue) and `fetch`/async APIs do NOT. Document what
> works in `docs/JS-SUPPORT.md` so users aren't surprised.

---

## Phase 7 — Stabilization

### Task 7.1: Real-page test matrix + fixes

**Files:** `~/Projects/netsurf-nanos/docs/TESTED-SITES.md`

- [ ] **Step 1:** Render a fixed list of content sites (example.com, a Wikipedia article, a plain
      blog, a docs page, the local darkhttpd `/apps/www`). For each: capture a screendump, note
      pass/glitch/crash.
- [ ] **Step 2:** For each crash, use the headless QEMU fault recipe (`-no-reboot -d int -D log`,
      grep `v=0d`/`v=0e`/`v=08`) to localize, fix in the relevant lib's port or the frontend, re-run.
- [ ] **Step 3:** For each layout glitch, check whether it's a charset issue (revisit
      libparserutils iconv, Task 2.2) or a missing CSS feature (libcss limitation — document, don't
      chase). Record outcomes in `TESTED-SITES.md`.
- [ ] **Step 4: Commit** the matrix + any fixes.

### Task 7.2: Memory + window sizing

**Files:** none new (tuning)

- [ ] **Step 1:** NetSurf is heavier than NanOS's usual apps. Confirm the user window/heap/mmap
      budgets in `arch/x86/mm/mmu_x86.cpp` (heap 32 MiB, mmap 64 MiB — see memory.md §4) are enough
      for a few open pages; if NetSurf OOMs, bump QEMU RAM (`make run` already gives NIC; raise `-m`)
      and/or the mmap window. Document the chosen RAM in the NanOS `make run` recipe.
- [ ] **Step 2:** Handle NanWM resize: when `NW_EV_CONFIGURE` arrives, NetSurf must re-fetch the
      window buffer (it may have moved) — extend `nanwm_input` to surface a `NSFB_EVENT_RESIZE` and
      re-`nw_win_surface` in the frontend's resize handler. Verify dragging a window edge re-lays-out.
- [ ] **Step 3: Commit.**

### Task 7.3: FreeType fonts (optional polish)

**Files:** `ports/freetype/nxport.toml`, modify `ports/netsurf/Makefile.config`

- [ ] **Step 1:** Only after everything works with the internal bitmap font: port FreeType
      (autotools, needs zlib + libpng optional) and flip `NETSURF_USE_FREETYPE := YES` +
      `NETSURF_FB_FONTLIB := freetype` for scalable, anti-aliased text. Ship a default TTF in `res/`.
- [ ] **Step 2: Verify + commit.**

---

## Self-Review (author's check against the design)

**Spec coverage** — the brainstorm settled on: NetSurf, NanWM client `.nxe`, separate repo,
multi-agent parallelism, HTTPS via the in-progress OpenSSL, JS optional/later. Mapping:
- Separate repo → Phase 0 (Task 0.1). ✔
- Multi-agent parallelism → "How to read this plan" (waves + isolation rule) + per-port independence. ✔
- Real CSS + images → Phases 1–2 (engine libs + image decoders) + Phase 4 render checks. ✔
- NanWM graphical client → Phase 3 (libnsfb NanWM backend) + Phase 5 (image bundle/`/bin`). ✔
- HTTPS → Task 1.5 (libcurl + OpenSSL) + Task 4.2 Step 4. ✔
- JS optional/later → Phase 6, explicitly gated and flagged partial. ✔
- NanOS integration as a repeatable target → Phase 5 (`make netsurf` + `_image` block, mirroring the
  existing `openssl` target). ✔
- **No shortcuts on foundations** (user requirement) → the "Foundations are done properly" rules +
  the 7-step template (cache/config audit + upstream test suite per base lib) + a real `libiconv`
  port (Task 1.6) replacing the earlier built-in-charset stopgap, with libparserutils wired to it. ✔

**Known soft spots (intentionally left as in-task iteration, not placeholders):**
- Per-port `cache` (gnulib forcings) is **audited, not guessed** (template Step 4): build, read the
  generated config, confirm each decision, annotate each forcing with `# why:`. The existing
  wget/inetutils ports are the reference for the idiom — genuine cross-compile work, not a stub.
- Exact libnsfb internal symbol names + the NetSurf option-var spellings must be confirmed against the
  pulled checkouts — each such spot is called out with the file to copy from. This is unavoidable for
  a port against upstream HEAD; the skeletons are structurally faithful.

**Type/name consistency** — `NSFB_SURFACE_NANWM`, `nanwm_rtns`, `nanwm_map_key`, `struct nanwm_priv`,
`scripts/env.sh` paths, `install = "sysroot"` + `sysroot_libs`/`sysroot_headers`, and the `make
netsurf` → `_image` flow are used consistently across tasks.

---

## Dependency-order quick reference (for `scripts/build-all.sh`)

```
# Wave 0:  zlib libjpeg expat libiconv libwapcaplet libnsutils libnslog \
#          libnsgif libnsbmp libnsfb(core) libcurl duktape  (_nsbuild + nsgenbind first)
# Wave 1:  libpng(zlib) libparserutils(libiconv)
# Wave 2:  libhubbub(parserutils,wapcaplet) libcss(parserutils,wapcaplet)
# Wave 3:  libdom(hubbub,parserutils,wapcaplet,expat)
# Wave 4:  libnsfb(+NanWM backend, rebuild over core)   [frontend-nanwm]
# Wave 5:  netsurf(all of the above + libnw)
# Wave 6:  netsurf(JS) = duktape + nsgenbind + rebuild
```
`build-all.sh` installs each into the shared sysroot in this order; `--until <lib>` stops early (used
to seed a temp sysroot for isolated per-port verification).
```
```

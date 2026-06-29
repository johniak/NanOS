---
name: nanos-netsurf-browser-port
description: NetSurf browser on NanOS — DONE & BROWSES THE LIVE WEB: renders real http+https sites (example.com over TLS) in a NanWM window, URL-bar navigation works. 14 libs + NanWM libnsfb backend + netsurf.nxe + image install + network all done
metadata: 
  node_type: memory
  type: project
  originSessionId: d9f3d2b3-9551-4707-bd67-d80a786155b3
---

Decided graphical web browser for NanOS = **NetSurf** (own C engine, no threads, no GPU — the only
mainstream engine that fits; WebKit/Gecko/Blink are blocked by NanOS having no threads + no GPU, and
needing libstdc++/Rust-std it lacks). Renders real CSS 2.1 + images; **no practical JS** (Duktape
gives partial ES5 as an optional later phase). HTTPS via the in-progress OpenSSL port + libcurl.

Key constraints discovered: i686-nanos toolchain has **no libstdc++** and **no threads** (no
pthread.h / no clone) — that's what rules out C++-STL engines (litehtml) and all JS-engine-with-DOM
browsers.

Architecture: a **separate repo `~/Projects/netsurf-nanos`** holds per-library `nxport.toml` ports
(install into the SDK sysroot) + a new **libnsfb surface backend** that draws into a NanWM window
buffer (`nw_surface`, 32bpp BGRX) and maps `nw_next_event` → nsfb events. NanOS consumes it via a
`make netsurf` target (mirrors `make openssl`), installs to `/apps/netsurf` + `/bin` symlink.

Plan (agent-parallelizable, dependency waves): `docs/superpowers/plans/2026-06-13-netsurf-nanos-port.md`.
Foundations done properly — see [[no-shortcuts-on-foundations]].

**EXECUTED 2026-06-14 (multi-agent, repo `~/Projects/netsurf-nanos`, committed there):**
- Phase 0: scaffold + orchestration scripts. Two build mechanisms PROVEN: tarball→sysroot (zlib)
  and NetSurf-buildsystem→sysroot (libwapcaplet). See `docs/PORT-RECIPE.md` (the verified recipe).
- KEY MECHANISM FACTS: builds run in `nanos-sdk-dev` container, sysroot = bind-mounted
  `/work/toolchain/i686-nanos`. Container HAS git (use `git://git.netsurf-browser.org/<lib>.git`)
  but NO wget/curl/gperf → tarballs fetched on HOST via `scripts/fetch.sh` (→`source="dir:src"`,
  gitignored); gperf installed in-container via a pre_configure apt hook. The nanos-port driver's
  built-in `sysroot_headers` copy FLATTENS via basename (breaks `<curl/curl.h>`) → every lib
  installs via a `hooks/post_build.sh` (`make install` into the sysroot) with `install="sysroot"`
  (skips mknx) + empty sysroot_libs/headers — exactly like the openssl port.
- Phases 1-3 (Waves 0-3, all built+smoke-linked, parallel agents): zlib, libjpeg-turbo(SIMD off),
  expat, GNU libiconv(full encodings; overwrites picolibc iconv.h — intended), libcurl(OpenSSL
  HTTPS, sync resolver=no threads, +`-DPOLLPRI=0x002` shim), libwapcaplet, libns{utils,log,gif,bmp},
  libnsfb(core), libpng(SSE off: QEMU CPU has no SSE2), libparserutils(REAL GNU libiconv backend,
  not 8859 stub), libhubbub, libcss(int32_t==long ABI: -Wno-error=incompatible-pointer-types +
  (css_fixed_or_calc)(css_fixed)0 union-cast, idempotent pre_configure patches survive re-clone),
  libdom(expat XML binding).
- **CRITICAL runtime fact discovered: the SDK .nxe runtime runs NO constructors** — crt0 doesn't
  call __libc_init_array, nx.ld has no .init_array section, __libc_init_array is just an import
  trampoline. So `__attribute__((constructor))` NEVER fires for ported programs. libnsfb surfaces
  self-register via constructors → fixed with explicit `_nsfb_register_builtins()` called from
  nsfb_new/nsfb_type_from_name/nsfb_enumerate_surface_types. Watch this for ANY ported lib using
  constructors (OpenSSL 3.0 uses explicit init, OK).
- Phase 3: bespoke NanWM libnsfb backend `frontend-nanwm/nsfb_nanwm.c` (NSFB_SURFACE_NANWM, format
  XRGB8888 = NanOS 0x00RRGGBB-LE; input maps NW_EV_KEY/POINTER/CLOSE/CONFIGURE→nsfb, mouse-button
  edges→NSFB_KEY_MOUSE_1/2/3) + keymap.h, injected into libnsfb via pre_configure.
- Phase 4: `netsurf.nxe` (6.7MB) BUILT — `make TARGET=framebuffer`, internal bitmap font, JS off,
  links all libs + libnw.ndl. No-shortcuts cross fixes: nanos_compat.h for PATH_MAX/NAME_MAX;
  `#undef HAVE_REALPATH/SCANDIR/FSTATAT/UNLINKAT` → NetSurf's own portable fallbacks (no feature
  cut); synthesised openssl.pc; -lnw.ndl after --no-whole-archive. Surface chosen at runtime `-f nanwm`.
- Phase 5 (NanOS repo, branch feat/nap-package-manager): `make netsurf` target + `_image`
  recursive res/ install + **disk image grown 32MB→256MB** (create-grub2-image.sh; NetSurf is 7MB).
- Render bring-up DONE: NetSurf runs ARGLESS under nwm (spawn_client passes no args), so the
  netsurf port bakes in default surface=nanwm + respath=/disks/main/apps/netsurf/res + start URL
  file:///.../res/welcome.html (pre_configure patches frontends/framebuffer/gui.c). post_build
  `cp -RL` (res/ is symlinks into resources/). user/nwm/nwm.c auto-spawns it on the desktop.
- LAST BLOCKER (fixed): HTML parse failed (HUBBUB_BADENCODING) because picolibc TABLE-ctype's
  `_ctype_b` is a libc.ndl DATA export; SDK static libs reference the RAW symbol but the .ndl
  loader only binds `__imp_`-prefixed slots → `_ctype_b`=0 → tolower('U')=='U' → charset lookup
  fails. Fix: `ports/_nsbuild/nx-ctype.h` does `#define _ctype_b (*__imp__ctype_b)` (mirrors
  in-tree user/libc-glue/nx-dllimport.h), force-included before <ctype.h>. A sysroot-wide nm
  survey found ONLY libparserutils + the netsurf app reference the raw symbol (everything else
  uses out-of-line libc ctype) → fix wired into just those two pre_configure hooks. **PROPER
  SDK-side fix for the future: add this indirection to the SDK <ctype.h>, or build picolibc with
  _PICOLIBC_CTYPE_SMALL=1 (arithmetic ctype, no table) — affects ALL SDK ports using uppercase ctype.**
- VERIFIED: headless QEMU → nwm → NetSurf renders welcome.html fully; e2fsck-clean image, 0 faults.
- **NETWORK BROWSING DONE (2026-06-14):** renders real http://example.com AND https://example.com
  (TLS, cert-verified), URL-bar navigation works (typed http://neverssl.com → loaded). Two fixes:
  * **KERNEL ROOT CAUSE (committed to NanOS, kernel/Syscall.h + net/Socket.h):** the socket layer
    returned LINUX errno numbers but userland is picolibc/newlib (different numbering). Linux
    EINPROGRESS=115 == newlib ENETDOWN. Non-blocking connect() (libcurl's curl_multi path) returned
    -EINPROGRESS, userland saw errno 115='network down' → CURLE_COULDNT_CONNECT in ~25ms. Blocking
    wget worked (returns 0) which masked it. Re-mapped socket errnos to the target <sys/errno.h>.
    Host tests 555/555, 91.2% cov. **Lesson: kernel→userland errno MUST use picolibc/newlib numbers,
    not Linux — affects every picolibc net client using non-blocking connect.**
  * keymap.h: NanWM Enter is LF('\n'=10), libnsfb wants NSFB_KEY_RETURN=CR(13) → URL bar inserted
    a newline instead of navigating. Fold LF→RETURN.
  * CA bundle: nsoption ca_bundle=/disks/main/nanos/ssl/cert.pem (https verify, no options file).
  * NS_START_URL env selects the homepage at build (default = offline welcome.html). Nav gesture:
    click address bar, type URL WITH scheme (http://), Enter.
- FreeType 2.13.2 ported into the sysroot (full module set, scalable AA fonts) — library only;
  enabling NETSURF_USE_FREETYPE is a pending netsurf rebuild. Optional remaining (plan Ph 6-7):
  JS (Duktape+nsgenbind), wire FreeType into netsurf, NanWM resize (NW_EV_CONFIGURE→re-fetch
  surface), real-site test matrix.

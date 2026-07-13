# 00 - Chromium Source And GN Target

**Goal:** Fetch the exact Chromium revision required by the pinned Electron release and make GN
generate a NanOS build directory.

**Entry criteria:** the directory README's entry checklist is green (lock file, microtests, disk
space).

## Decisions

- **Compiler: Chromium's bundled clang, NOT `x86_64-nanos-gcc`.** Modern Chromium (the milestone
  Electron 42 pins) does not build with gcc in practice — upstream dropped gcc support around
  M110 and the tree assumes clang flags/attributes throughout. Use the bundled LLVM toolchain
  (`tools/clang/scripts/update.py` fetches it during `gclient sync`) with `--target=x86_64-nanos`
  and the SDK's headers/libs as the "sysroot": the `build/toolchain/nanos/BUILD.gn` clone below
  is therefore modeled on a **clang** gcc_toolchain instance (see how linux clang x64 is
  defined), with `-isystem`/`-L` pointing into the NanOS SDK. The SDK gcc still links fine as
  `ld` driver if lld fights the `.nxe` flow — record which linker won in
  `ports/chromium/README.md`. (Node, plan 02, may stay on nanos-gcc — gyp+gcc is still fine
  there.)
- **Real `target_os = "nanos"`, defined as "linux minus glibc/X11/DBus".** GN recognizes `nanos`
  as a first-class OS whose build config deliberately reuses POSIX/Linux code paths via
  `is_linux || is_nanos` conditionals where they are correct. This keeps patches greppable and
  honest without rewriting Linux code Chromium already has. (Node takes the lighter
  `--dest-os=linux + __NANOS__` route; Chromium can't, because Ozone platform registration and
  build flags key off `target_os`.)
- **`use_custom_libcxx = true`.** NanOS has no system C++ standard library for user space;
  Chromium's bundled libc++ builds from source against NanOS libc headers. Verify the SDK's C++
  story first (Step 2 below).
- **Sandbox off, GPU off, single canonical args file** — `scripts/electron/chromium-args-nanos.gn`
  in this repo, copied verbatim into `out/NanOS/args.gn` by the gen command.

## Files

**Create:**
- `scripts/electron/fetch-chromium.sh`
- `scripts/electron/chromium-args-nanos.gn`
- `ports/chromium/README.md`
- `ports/chromium/patches/0001-build-add-nanos-os.patch` (and follow-on numbered patches)

## Steps

- [ ] **Step 1: Read the pin.**

```sh
grep chromium_version manifest/electron-stack.lock   # e.g. 1xx.0.xxxx.xx
```

If absent, stop — go back to `00-platform-contract.md` (the program plan, not this file).

- [ ] **Step 2: Probe the SDK C++ runtime** (decides how much libc++ work is ahead). Compile a
  10-line C++ program with `x86_64-nanos-g++` using `std::string`, `new`, a thrown+caught
  exception, and RTTI (`typeid`). Run it in QEMU. Record in `ports/chromium/README.md` which of
  {exceptions, RTTI, static init, atexit} work. Chromium builds with `-fno-exceptions
  -fno-rtti`, so partial support is acceptable — but you must know what's real.

- [ ] **Step 3: Write `scripts/electron/fetch-chromium.sh`.** Contents (adapt paths, keep shape):

```sh
#!/usr/bin/env bash
# fetch-chromium.sh — reproducible pinned Chromium checkout for the NanOS port.
set -eu
REV="$(grep chromium_version manifest/electron-stack.lock | cut -d'"' -f2)"
SDK_WORK="${SDK_WORK:-$HOME/Projects/nanos-sdk-work}"
SRC="$SDK_WORK/chromium-src"

# depot_tools (gclient/gn/ninja) — pinned clone next to the source
[ -d "$SDK_WORK/depot_tools" ] || git clone --depth 1 \
    https://chromium.googlesource.com/chromium/tools/depot_tools "$SDK_WORK/depot_tools"
export PATH="$SDK_WORK/depot_tools:$PATH"

mkdir -p "$SRC" && cd "$SRC"
[ -f .gclient ] || gclient config --name src --unmanaged \
    https://chromium.googlesource.com/chromium/src.git
[ -d src ] || git clone https://chromium.googlesource.com/chromium/src.git src
cd src
git fetch origin "refs/tags/$REV" && git checkout -f "$REV"
gclient sync -D --no-history --revision "src@$REV"

# apply NanOS patches in order; a clean re-fetch must reproduce the working tree
for p in "$OLDPWD"/ports/chromium/patches/*.patch; do git apply --3way "$p"; done
echo "$REV" > NANOS_REVISION.txt
```

Run it once; expect the sync to take hours and ~80 GiB.

- [ ] **Step 4: Add `target_os = "nanos"` recognition.** All inside
  `$SDK_WORK/chromium-src/src`, captured as `0001-build-add-nanos-os.patch`:
  - `build/config/BUILDCONFIG.gn`: accept `target_os == "nanos"`; set `is_nanos = true`,
    `is_posix = true`, `is_linux = false`, `current_os` plumbing — model every edit on how
    the file treats an existing niche OS (search for `"aix"` or `"fuchsia"` to see the pattern);
  - `build/config/nanos/BUILD.gn` (new): compiler/link flags, include dirs, and libs for the
    NanOS SDK toolchain;
  - `build/toolchain/nanos/BUILD.gn` (new): clone of the **clang** x64 toolchain instance in
    `build/toolchain/linux/BUILD.gn`, using the bundled clang with `--target=x86_64-nanos` and
    SDK include/lib paths (see the compiler Decision above);
  - `build/config/BUILD.gn` + `build/config/features.gni`-family: add `is_nanos` to the defines
    plumbing so C++ sees `OS_NANOS` (mirror how `OS_LINUX`/`BUILDFLAG(IS_LINUX)` is emitted).
  - Host tools must keep building for the host: verify the host toolchain block is untouched
    (`gn gen` Step 6 fails loudly if not).

- [ ] **Step 5: Create the canonical `scripts/electron/chromium-args-nanos.gn`:**

```gn
# Canonical NanOS GN args — the ONLY copy; see 03-chromium-content/README.md rules.
# If the pinned milestone rejects an arg name, delete/rename it here and record why in
# ports/chromium/README.md (arg names drift between milestones; intent below is the contract).
target_os = "nanos"
target_cpu = "x64"
is_debug = false
is_component_build = false
symbol_level = 1
blink_symbol_level = 0
treat_warnings_as_errors = false
cc_wrapper = "ccache"           # Chromium rebuilds are brutal without it; install ccache first

use_sysroot = false
use_custom_libcxx = true        # NanOS has no system libc++ (see subplan Decisions)

# allocator: no shim, plain malloc — exact arg names depend on milestone
use_partition_alloc_as_malloc = false
use_allocator_shim = false

use_ozone = true
ozone_auto_platforms = false
ozone_platform = "nanos"
ozone_platform_nanos = true
use_aura = true

# no Linux desktop stack
ozone_platform_x11 = false
ozone_platform_wayland = false
use_glib = false
use_gtk = false
use_dbus = false
use_udev = false
use_alsa = false
use_pulseaudio = false
use_cups = false

enable_nacl = false
enable_printing = false
enable_extensions = false       # revisit for generic Electron apps if required
is_cfi = false
v8_enable_sandbox = false
enable_vulkan = false
```

- [ ] **Step 6: Generate.**

```sh
export PATH="$SDK_WORK/depot_tools:$PATH"
cd "$SDK_WORK/chromium-src/src"
gn gen out/NanOS --args="$(cat /path/to/repo/scripts/electron/chromium-args-nanos.gn)"
```

Expected: `Done. Made XXXXX targets from YYYY files in ZZZZms`. Iterate on GN/config errors only
— **do not start porting C++ code in this subplan**. Each unknown-arg or config error is fixed in
the args file or patch 0001 and committed.

- [ ] **Step 7: Document.** `ports/chromium/README.md`: exact revision, fetch command, gen
  command, C++ probe results (Step 2), and the args-drift log.

## Gate

```sh
scripts/electron/fetch-chromium.sh          # idempotent on an existing checkout
cd "$SDK_WORK/chromium-src/src" && gn gen out/NanOS \
  --args="$(cat /path/to/repo/scripts/electron/chromium-args-nanos.gn)" && echo GN-OK
```

- [ ] `GN-OK` prints from a clean (re-fetched) checkout.
- [ ] `ports/chromium/README.md` documents revision + commands + probe results.
- [ ] `status.md` row added; parent coordinator checkbox 00 ticked.

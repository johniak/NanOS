# The NanOS ecosystem — repos, dependencies, reproducibility

Everything NanOS-related lives in the **[`NanOS-labs`](https://github.com/NanOS-labs)**
GitHub organization. Every upstream project we modify is a **fork carrying its own port
recipe** (`nxport.toml`, `hooks/`, build scripts) so a fresh machine can rebuild the whole
OS — kernel, userland, every port, drivers, the GL desktop and the host GL stack — from
nothing but these repos. Spec: `docs/superpowers/specs/2026-07-12-ecosystem-reproducibility-design.md`.

The single machine-readable source of truth for "what checks out where" is
[`nanos-sdk/ports.manifest`](https://github.com/NanOS-labs/nanos-sdk/blob/master/ports.manifest);
`nanos-sdk/scripts/bootstrap.sh` consumes it.

## Fresh machine → working OS

```sh
git clone git@github.com:NanOS-labs/nanos-sdk.git ~/Projects/nanos-sdk
git clone git@github.com:NanOS-labs/NanOS.git     ~/Projects/NanOS
~/Projects/nanos-sdk/scripts/bootstrap.sh    # host checks, clone all forks, toolchain, docker
cd ~/Projects/NanOS
make world                               # everything below in one command
make run64                               # boot it (SMP, cocoa display)
make flash-dell                          # or write the Dell USB stick
```

`make world` runs the full layered recipe in dependency order — equivalent to:

```sh
make docker-image
make zlib openssl ncurses                # library layer
make toybox sudo grep bzip2              # base userland
make ping wget inetd httpd udhcpc dropbear   # network layer
make vim htop git sqlite bash            # apps
make libpng libjpeg                      # image codecs
make mesa-intel-clc                      # first run only (host tool for iris)
make libdrm mesa gles2info glkms nwm-gl  # GL userspace
make externals && make image64           # stage everything + the disk image
```

Every layer is also a standalone target for selective rebuilds.

x86_64 is the only architecture (`ARCH` defaults to it) — the i686/32-bit flow was retired
on 2026-07-13 (`docs/superpowers/plans/2026-07-13-i686-retirement.md`): no `arch/x86`, no
32-bit image/userland targets, no i686-elf toolchain in the build container, no i686-nanos
SDK toolchain. The whole org was audited the same day: `nanos-port` and every fork recipe
(hooks/nxport.toml/build scripts) now default to the `x86_64-nanos` triple (env-driven
`NX_HOST` behaviour unchanged). Two follow-up ports remain from the i686 era: **NetSurf**
(`make netsurf` errors with a pointer) and the **nap package-manager client**
(`nano-packages/client` still targets the retired i686-nanos Rust spec).

`bootstrap.sh` is idempotent — a half-finished run resumes where it stopped. Override the
workspace location with `SDK_WORK=...` (that is how the clean-room gate runs).

### macOS host GL stack (optional — `make run64-gl`, `run64-gl-desktop`)

GPU-accelerated guest GL needs a virgl-enabled QEMU, which Homebrew does not ship:

```sh
brew tap startergo/libepoxy startergo/angle && brew install startergo/libepoxy/libepoxy startergo/angle/angle
git clone git@github.com:NanOS-labs/virglrenderer-nanos.git && sh virglrenderer-nanos/nanos/build-virgl.sh
git clone git@github.com:NanOS-labs/qemu-nanos.git && sh qemu-nanos/nanos/build-qemu.sh
```

Point the Makefile at the result with `QEMU_GL=/path/to/qemu-system-x86_64` (default:
`$(SDK_WORK)/qemu-virgl-kosmickrisp/bin/qemu-system-x86_64`). Note the brew-tap dylibs lose
their code signature on bottle relocation; the `run64-gl*` targets re-sign them ad hoc
(`codesign --force --sign -`).

## Repo table

Conventions: default branch `nanos`; forks with a pristine-tarball base carry an
`upstream-base` tag; forks imported from the live working tree instead document their diff
against upstream in `docs/UPSTREAM-DIFF.md` (generated during the 2026-07-12 migration
audit). Checkout paths are relative to `$SDK_WORK` (`~/Projects/nanos-sdk-work`).

### Core

| Repo | What | Consumed by |
|---|---|---|
| `NanOS` | The OS: kernel, drivers, userland, compositor, images | everything |
| `nanos-sdk` | Cross toolchain (binutils 2.43 + gcc 14.2.0 patched for x86_64-nanos via `toolchain/patch.sh`; the retired i686-nanos hunks stay in patch.sh by design), picolibc sysroot, the `nanos-port` driver, `ports.manifest`, `bootstrap.sh`, `migrate-fork.sh` | all ports |
| `nano-packages` | "nap" — the NanOS package manager (Rust client + Django registry). Design/plan in its `docs/`; client targets the retired i686-nanos spec — x86_64 port is a follow-up | standalone |

### Ports (checkout path → make target)

| Repo | Upstream base | NanOS changes | Checkout | Target |
|---|---|---|---|---|
| `zlib-nanos` | zlib 1.3.1 | recipe only | `zlib-port` | `make zlib` |
| `openssl-nanos` | OpenSSL 3.0.15 | recipe (+generated docs in tree) | `openssl-port` | `make openssl` |
| `dropbear-nanos` | Dropbear 2022.83 | recipe | `dropbear-port` | `make dropbear` |
| `git-nanos` | git 2.54.0 | **`run-command.c` .nxe exec resolution** + `config.mak` + recipe | `git-port` | `make git` |
| `htop-nanos` | htop 3.5.1 | recipe | `htop-port` | `make htop` |
| `wget-nanos` | wget 1.21.4 | recipe | `wget-port` | `make wget` |
| `libpng-nanos` | libpng 1.6.43 | recipe (sysroot lib) | `libpng-port` | `make libpng` |
| `libjpeg-nanos` | IJG libjpeg 8d | recipe (sysroot lib) | `libjpeg-port` | `make libjpeg` |
| `darkhttpd-nanos` | darkhttpd 1.17 | recipe | `darkhttpd-port` | `make httpd` |
| `ncprobe` | (own code) | NanOS-native console network probe | `ncprobe-port` | — |
| `inetutils-nanos` | GNU inetutils 2.5 | two recipes: ping + services (ifconfig/telnet/telnetd/traceroute/inetd) | `inetutils-port` (+`inetutils-services-port` symlink) | `make ping`, `make inetd` |
| `toybox-nanos` | toybox 0.8.11 (tag `upstream-base`) | pristine + recipe-in-container | `toybox-0.8.11` | `make toybox` |
| `sudo-nanos` | sudo 1.9.15p5 (tag) | pristine | `sudo-1.9.15p5` | `make sudo` |
| `grep-nanos` | GNU grep 3.11 (tag) | configured-tree state | `grep-3.11` | `make grep` |
| `bzip2-nanos` | bzip2 1.0.8 (tag) | pristine | `bzip2-1.0.8` | `make bzip2` |
| `busybox-nanos` | busybox 1.36.1 (tag) | `.config` (udhcpc) + build state | `busybox-1.36.1` | `make udhcpc` |
| `vim-nanos` | vim (upstream fork) | recipe at source root (x86_64 flow, committed cross config.cache) | `vim` symlink + `vim-port` | `make vim` |
| `ncurses-nanos` | ncurses (mirror fork) | recipe at source root; libtinfo/fallback-terminfo strategy | `ncurses-port/src` (+recipe copied up) | `make ncurses` |
| `bash-nanos` | GNU bash 5.2 (savannah) | NanOS build in `nanos/` | `~/Projects/bash-nanos` | `make bash` |
| `netsurf-nanos` | NetSurf | NanWM libnsfb backend + ports (i686-era; awaiting the browser-on-x86_64 follow-up port) | `~/Projects/netsurf-nanos` | — |
| `sqlite-nanos` | SQLite 3.46.1 amalgamation | `nanos/` build + compat + sqltest | `~/Projects/sqlite-nanos` | `make sqlite` |

### Graphics / GL

| Repo | Upstream base | NanOS changes | Checkout | Target |
|---|---|---|---|---|
| `libdrm-nanos` | libdrm 2.4.123 | 1 patch (drmDevice without sysfs) + recipe | `libdrm-port` | `make libdrm` |
| `mesa-nanos` | Mesa 24.2.8 | 4 patches (virgl build-id, legacy fence, i915→iris loader map, kcmp) + meson/port glue + `build-nwm-gl.sh`/`glkms`/`gles2info` recipes | `mesa-port` | `make mesa`, `make nwm-gl` |
| `qemu-nanos` | QEMU master snapshot (tag `upstream-base`) | commit-per-fix: cocoa GL scanout, ui/console dispatch, virtio-gpu-virgl + the recovered texture-borrowing patchset | `qemu-fork-build` | host `run64-gl*` |
| `virglrenderer-nanos` | virglrenderer 1.3.0 (tag) | 34 kosmickrisp patches **vendored** (20 applied per `nanos/build-virgl.sh`), recovered vrend scanout changes; `drain` branch = scanout drain experiment | `virgl-fork-build` | host `run64-gl*` |

## Host dependencies (versions verified 2026-07-12)

- Docker 29.4.0 (both build containers: `nanos-build` from `NanOS/docker/`, `nanos-sdk-dev` from `nanos-sdk/Dockerfile`)
- Homebrew: qemu 11.0.2 (plain `run64`; needs `-display cocoa,zoom-to-fit=on` on Retina), e2fsprogs 1.47.3, python3 + Pillow 12.2.0 (assets), shellcheck 0.11.0 (dev)
- Homebrew taps for the host GL stack: startergo angle 1.0.16, libepoxy 1.0.5 (the tap's virglrenderer 1.0.41 is superseded by `virglrenderer-nanos`)

## Not forked, by design

- **gcc 14.2.0 / binutils 2.43 / picolibc** — materialized from upstream releases by
  `nanos-sdk` (`toolchain/patch.sh` applies the nanos target; the Dockerfile clones picolibc
  at a pinned ref). The workspace trees were audited 2026-07-12: modifications match `patch.sh`.
- **musl 1.2.5** — workspace tree verified byte-identical to the upstream release
  (it seeded the in-tree pthread port; nothing to fork).
- **startergo angle/libepoxy** — host-side dylibs consumed as brew bottles; versions pinned
  above, re-sign quirk documented. Not our code.
- **`~/Projects/git`** — clean upstream clone used for reference only.

## Clean-room gate verdict (2026-07-13): **GREEN**

`SDK_WORK=/tmp/fresh-sdk-work bootstrap.sh` + the full make sequence, sourcing ONLY the
NanOS-labs org (toolchain rsync-seeded from the live workspace — its from-source build is
covered by the nanos-sdk flow):

- **Every stage passes**: docker-image + zlib, openssl, ncurses, toybox, sudo, grep, bzip2,
  ping, wget, inetd (+telnetd/telnet/ifconfig/traceroute), httpd, udhcpc, dropbear, vim,
  htop, git, sqlite, bash, libpng, libjpeg + the GL chain (libdrm, mesa, gles2info, glkms,
  nwm-gl) + externals (**14/14 apps staged**) + **image64 + image64-gl**.
- **`verify64` fully green clean-room**: host tests (3.25M assertions, coverage ≥90%) +
  BIOS/UEFI/big-RAM boots + e1000e MSI + live-USB (+SMP/storm) + VT switch + virtio-gpu +
  i915 + all four SMP gates. Final image re-verified with smoke-x86_64/usb/vt/virtio-gpu.
- The 2026-07-12 first run had exposed a 15-port pre-existing rebuild rot (autotools EXEEXT
  family from crt0's script-symbol refs, hand-patched-but-unversioned sysroot headers,
  i686-hardwired Makefile targets, per-port glue drift). All of it was fixed on 2026-07-13 —
  root causes and fixes are recorded in the NanOS/nanos-sdk commit history (a8cd954,
  3596157, 772f595, cb04e20 + per-fork `nanos:` commits).
- **Known exclusion**: NetSurf builds only through the legacy i686 flow, which is dead at
  the libc level (the pthread/TLS layer is x86_64-only) and scheduled for deletion; the
  browser was never part of image64. **Browser-on-x86_64 is the documented follow-up port.**

## Provenance notes

- Migration + audit tooling: `nanos-sdk/scripts/migrate-fork.sh` (pristine-base or
  tree-import, with a printed audit of every in-place modification) and the
  `docs/UPSTREAM-DIFF.md` files in each tree-imported fork.
- The audit recovered two silent patch sets that existed nowhere else: git's
  `run-command.c` .nxe resolution and QEMU's texture-borrowing GL series — both now
  committed history.

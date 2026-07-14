# Architecture selection: machine-dependent knobs come from arch/$(ARCH)/arch.mk
# (CROSS, ARCH_VPATH, ARCH_INCLUDES, ARCH_LINKER, ARCH_SOURCES).
ARCH ?= x86_64
include arch/$(ARCH)/arch.mk

# Machine-independent objects (portable across architectures).
MI_SOURCES=kmain.o Kernel.o Console.o ExtFilesystem.o SynthFs.o RamFs.o RamBlockDevice.o DeviceManager.o Vfs.o
MI_SOURCES+= Crc32c.o BlockCache.o ExtCsum.o ExtAllocator.o Journal.o
MI_SOURCES+= Framebuffer.o Font8x16.o FbConsole.o vtk.o Fbdev.o Fb0Device.o DrmDevice.o NwShmDevice.o KeyboardDevice.o Pty.o
MI_SOURCES+= VtConsole.o VtManager.o VtTty.o   # virtual terminals: per-VT console + switching manager + /dev/ttyN
MI_SOURCES+= Syscall.o Cred.o SyscallDispatch.o NxeLoader.o Exec.o DynLoader.o KernelExports.o KextLoader.o FrameAllocator.o KeyDecoder.o Scheduler.o Process.o Signal.o Futex.o Csprng.o
MI_SOURCES+= Pci.o MsiRouter.o Acpi.o Bkl.o
MI_SOURCES+= Net.o NetBuf.o NetDevice.o Loopback.o NetCore.o Ether.o Arp.o Ip.o Route.o Icmp.o
MI_SOURCES+= Socket.o Udp.o Raw.o Tcp.o Packet.o Unix.o NetProc.o NetStats.o
MI_SOURCES+= UsbCore.o UsbHid.o UsbMsc.o UsbMscBlockDevice.o UsbHidInput.o MouseDevice.o
MI_SOURCES+= PartitionTable.o
MI_SOURCES+= memory_manager.o Heap.o List.o String.o icxxabi.o string_funcs.o
# Full link set = portable objects + the selected arch's machine-dependent objects.
SOURCES=$(MI_SOURCES) $(ARCH_SOURCES)

BINFOLDER=bin/
# The x86_64 disk image (hybrid GPT + Limine, BIOS+UEFI).
IMAGE64=disk/image64.img
# Hybrid GPT+Limine layout: the ext4 root (P3) starts at 34 MiB (after bios_boot @1MiB + ESP @2MiB,32MiB).
IMAGE64_PART=$(IMAGE64)?offset=69206016

DOCKER_IMAGE=nanos-build
# Build the image for the host's NATIVE architecture (no --platform): the x86_64-elf cross
# toolchain and all image tools are arch-agnostic, so on Apple Silicon this runs natively
# instead of under QEMU amd64 emulation — the single biggest build speedup. The output
# is identical regardless of build-host arch.
DOCKER_RUN=docker run --rm -v $(CURDIR):/src -w /src $(DOCKER_IMAGE)

ifeq ($(wildcard /etc/nanos-build),)
# ============================================================================
# HOST side (macOS): build inside Docker, run QEMU natively.
# ============================================================================

docker-image:
	docker build -t $(DOCKER_IMAGE) docker/

# GNU bash (optional, external). The bash sources AND its build scaffolding (nx-gcc wrapper,
# build.sh, config.cache, port patches) live in a SEPARATE fork repo — NOT here. This target
# only runs that build inside the cross-toolchain container (both repos bind-mounted) and
# copies the resulting bash.nxe into bin/, where _image64 installs it as the /apps/bash bundle.
# Absent fork => the target errors clearly; `make image` itself never depends on this, so a
# missing fork can't break a normal build.
BASH_FORK ?= $(HOME)/Projects/bash-nanos
# The fork builds against the 64-bit picolibc sysroot + x86_64-elf-gcc, links the elf64 .nxe
# linker script and turns the ELF into a v4 .nxe with mknx64 (NX_LP64=1 fixes bash's LP64
# sizeof answers).
BASH_ENV = -e NX_CC=x86_64-elf-gcc -e NX_HOST=x86_64-elf -e NX_PICO=/opt/picolibc/x86_64-elf \
  -e NX_MKNX=/src/bin/mknx64 -e NX_LDSCRIPT=/src/arch/x86_64/user-nx.ld \
  -e 'NX_ARCHFLAGS=-mcmodel=small -mno-red-zone' -e NX_LP64=1
# The fork links /src/bin/{crt0.o,nxhdr.o,libc.ndl.a} + mknx64 — they must be the ELF64 build.
BASH_PREREQ = $(DOCKER_RUN) sh -c 'make ARCH=x86_64 bin/libc.ndl bin/libc.ndl.a bin/crt0.o bin/nxhdr.o bin/mknx64'
bash: docker-image
	@test -f "$(BASH_FORK)/nanos/build.sh" || { echo "bash fork not found at $(BASH_FORK)/nanos (set BASH_FORK=/path/to/bash-nanos)"; exit 1; }
	$(BASH_PREREQ)
	docker run --rm -v $(CURDIR):/src -v "$(BASH_FORK)":/bash $(BASH_ENV) -w /bash $(DOCKER_IMAGE) sh /bash/nanos/build.sh build
	cp "$(BASH_FORK)/nanos/bash.nxe" $(BINFOLDER)bash.nxe
	@echo "staged $(BINFOLDER)bash.nxe — run 'make image64' to install it"

# GNU grep / vim / bzip2 (optional, external). The upstream sources live in the nanos-sdk work
# dir. Each rebuilds its .nxe from source inside the nanos-build container via the reproducible
# driver scripts/nx-port-build.sh (nx-gcc wrapper -> picolibc + libc-glue + a NanOS .nxe link,
# mknx64 -> v4 .nxe). Mirrors `make bash`: the same NX_* env selects the toolchain, linker
# script, mknx and LP64 sizes; the driver applies the per-app LP64/POSIX fixes.
SDK_WORK ?= $(HOME)/Projects/nanos-sdk-work
# Shared x86_64 build env + the ELF64 startup/libc prereqs (same set `make bash` rebuilds).
NXPORT_ENV = -e NX_CC=x86_64-elf-gcc -e NX_HOST=x86_64-elf -e NX_PICO=/opt/picolibc/x86_64-elf \
  -e NX_MKNX=/src/bin/mknx64 -e NX_LDSCRIPT=/src/arch/x86_64/user-nx.ld \
  -e 'NX_ARCHFLAGS=-mcmodel=small -mno-red-zone' -e NX_LP64=1
NXPORT_PREREQ = $(DOCKER_RUN) sh -c 'make ARCH=x86_64 bin/libc.ndl bin/libc.ndl.a bin/crt0.o bin/nxhdr.o bin/mknx64'
# docker run that mounts BOTH this checkout (/src) and the app source ($1 -> /work/src).
NXPORT_RUN = docker run --rm -v $(CURDIR):/src $(NXPORT_ENV) -w /src

grep:
	$(NXPORT_PREREQ)
	$(NXPORT_RUN) -v "$(SDK_WORK)/grep-3.11":/work/src $(DOCKER_IMAGE) sh /src/scripts/nx-port-build.sh grep
	cp "$(SDK_WORK)/grep-3.11/grep.nxe" $(BINFOLDER)grep.nxe
	@echo "staged $(BINFOLDER)grep.nxe — run 'make image64' to install it into /nanos/bin"

# toybox (0BSD multicall): the user-identity tools (login/su/passwd/id/groups/whoami).
# Built via the nanos-sdk port flow; installed as a single setuid-root toybox.nxe
# with a per-command symlink farm (image64 install). Source: $(SDK_WORK)/toybox-0.8.11.
.PHONY: toybox
toybox:
	$(NXPORT_PREREQ)
	$(NXPORT_RUN) -v "$(SDK_WORK)/toybox-0.8.11":/work/src $(DOCKER_IMAGE) sh /src/scripts/nx-port-build.sh toybox
	cp "$(SDK_WORK)/toybox-0.8.11/toybox.nxe" $(BINFOLDER)toybox.nxe
	@echo "staged $(BINFOLDER)toybox.nxe — run 'make image64' to install it + the login/su/passwd/id symlinks"

# sudo (Todd Miller, ISC license): real sudo with the sudoers policy linked statically. x86_64
# only. Installed setuid-root in /nanos/bin; /etc/sudoers (%wheel) is seeded already. Source:
# $(SDK_WORK)/sudo-1.9.15p5.
.PHONY: sudo
sudo:
	$(NXPORT_PREREQ)
	$(NXPORT_RUN) -v "$(SDK_WORK)/sudo-1.9.15p5":/work/src $(DOCKER_IMAGE) sh /src/scripts/nx-port-build.sh sudo
	cp "$(SDK_WORK)/sudo-1.9.15p5/sudo.nxe" $(BINFOLDER)sudo.nxe
	@echo "staged $(BINFOLDER)sudo.nxe — run 'make image64' to install it setuid-root into /nanos/bin"

# Vim (the editor): built from source via the REPRODUCIBLE nanos-port driver (mirrors `make
# ping`/`make ncurses`) against the x86_64-nanos sysroot, linking libtinfo.a from the ncurses
# port. The manifest ($(VIM_PORT)/nxport.toml) reuses the committed vim_cv_* cross-cache;
# hooks/pre_configure.sh scrubs stale build state the `dir:` copy carries over. We first
# refresh the SDK sysroot
#     from THIS checkout (POSIX headers + libc.ndl{,.a} + the nx-dllimport.h data-import shim + the
#     crt0/nxhdr startup objects + mknx64) so the x86_64 build tracks the live ABI. Requires `make
#     ARCH=x86_64 ncurses` first (libtinfo.a + curses.h/term.h in the sysroot). `make image*` never
#     depends on this; a missing artifact is skipped by _image/_image64.
VIM_PORT := $(SDK_WORK)/vim-port
VIM_SRC  := $(SDK_WORK)/vim
VIM_TRIPLE   := x86_64-nanos
# CFLAGS = NanOS x86_64 user ABI (fixed-base ET_EXEC, small model, no red zone) + the dllimport shim
# force-included into every TU (x86_64 references picolibc's stdout/stderr/errno DATA RIP-relative,
# which mknx routes through __imp_<name> IAT slots). LDFLAGS=-no-pie keeps the fixed-base layout.
VIM_PORT_ENV  = -e NX_HOST=x86_64-nanos -e NX_LP64=1 \
  -e CFLAGS="-O2 -fno-pie -mcmodel=small -mno-red-zone -include nx-dllimport.h" \
  -e LDFLAGS="-no-pie"
vim:
	$(NXPORT_PREREQ)
	@test -d "$(SDK_TC)/$(VIM_TRIPLE)/include" || { echo "nanos-sdk $(VIM_TRIPLE) toolchain not found at $(SDK_TC)"; exit 1; }
	@test -f "$(SDK_TC)/$(VIM_TRIPLE)/lib/libtinfo.a" || { echo "libtinfo.a not in the $(VIM_TRIPLE) sysroot — run 'make ARCH=x86_64 ncurses' first"; exit 1; }
	@test -f "$(VIM_PORT)/nxport.toml" || { echo "vim port not found at $(VIM_PORT)/nxport.toml"; exit 1; }
	cp -R user/libc-glue/include/. "$(SDK_TC)/$(VIM_TRIPLE)/include/"
	cp kernel/SyscallNr.h            "$(SDK_TC)/$(VIM_TRIPLE)/include/SyscallNr.h"
	cp user/libc-glue/nx-dllimport.h "$(SDK_TC)/$(VIM_TRIPLE)/include/nx-dllimport.h"
	cp $(BINFOLDER)libc.ndl.a        "$(SDK_TC)/$(VIM_TRIPLE)/lib/libc.a"
	cp $(BINFOLDER)libc.ndl          "$(SDK_TC)/$(VIM_TRIPLE)/lib/libc.ndl"
	cp $(BINFOLDER)crt0.o            "$(SDK_TC)/$(VIM_TRIPLE)/lib/crt0.o"
	cp $(BINFOLDER)nxhdr.o           "$(SDK_TC)/$(VIM_TRIPLE)/lib/nxhdr.o"
	cp $(BINFOLDER)mknx64            "$(SDK_TC)/bin/$(VIM_TRIPLE)-mknx"
	docker run --rm \
	  -v "$(SDK_TC)":/work/toolchain -v "$(VIM_PORT)":/work/port -v "$(VIM_SRC)":/work/vim -v "$(NANOS_SDK)":/sdk \
	  -e SDK=/sdk $(VIM_PORT_ENV) -e PATH="/work/toolchain/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
	  -w /work/port nanos-sdk-dev:latest python3 /sdk/port/nanos-port /work/port
	cp "$(VIM_PORT)/vim.nxe" $(BINFOLDER)vim.nxe
	@echo "staged $(BINFOLDER)vim.nxe — run 'make image64' to install it into /apps/vim"

# htop (interactive process monitor). x86_64-ONLY (like the vim port): builds from source via the
# reproducible nanos-port driver against the x86_64-nanos sysroot, linking libncurses.a/libtinfo.a
# from the ncurses port. hooks/pre_configure.sh maps *nanos* to htop's Linux /proc backend, which
# reads NanOS's Linux-style /proc (SynthFs). Requires `make ARCH=x86_64 ncurses` first. `make
# image*` never depends on this; a missing artifact is skipped by _image64.
HTOP_PORT := $(SDK_WORK)/htop-port
HTOP_TRIPLE := x86_64-nanos
htop:
	$(NXPORT_PREREQ)
	@test -d "$(SDK_TC)/$(HTOP_TRIPLE)/include" || { echo "nanos-sdk $(HTOP_TRIPLE) toolchain not found at $(SDK_TC)"; exit 1; }
	@test -f "$(SDK_TC)/$(HTOP_TRIPLE)/lib/libtinfo.a" || { echo "libtinfo.a not in the $(HTOP_TRIPLE) sysroot — run 'make ARCH=x86_64 ncurses' first"; exit 1; }
	@test -f "$(HTOP_PORT)/nxport.toml" || { echo "htop port not found at $(HTOP_PORT)/nxport.toml"; exit 1; }
	cp -R user/libc-glue/include/. "$(SDK_TC)/$(HTOP_TRIPLE)/include/"
	cp kernel/SyscallNr.h            "$(SDK_TC)/$(HTOP_TRIPLE)/include/SyscallNr.h"
	cp user/libc-glue/nx-dllimport.h "$(SDK_TC)/$(HTOP_TRIPLE)/include/nx-dllimport.h"
	cp $(BINFOLDER)libc.ndl.a        "$(SDK_TC)/$(HTOP_TRIPLE)/lib/libc.a"
	cp $(BINFOLDER)libc.ndl          "$(SDK_TC)/$(HTOP_TRIPLE)/lib/libc.ndl"
	cp $(BINFOLDER)crt0.o            "$(SDK_TC)/$(HTOP_TRIPLE)/lib/crt0.o"
	cp $(BINFOLDER)nxhdr.o           "$(SDK_TC)/$(HTOP_TRIPLE)/lib/nxhdr.o"
	cp $(BINFOLDER)mknx64            "$(SDK_TC)/bin/$(HTOP_TRIPLE)-mknx"
	docker run --rm \
	  -v "$(SDK_TC)":/work/toolchain -v "$(HTOP_PORT)":/work/port -v "$(NANOS_SDK)":/sdk \
	  -e SDK=/sdk -e NX_HOST=x86_64-nanos -e NX_LP64=1 \
	  -e CFLAGS="-O2 -fno-pie -mcmodel=small -mno-red-zone -include nx-dllimport.h -include nx-getopt-import.h" \
	  -e LDFLAGS="-no-pie" -e LIBS="-lncurses -ltinfo" \
	  -e PATH="/work/toolchain/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
	  -w /work/port nanos-sdk-dev:latest python3 /sdk/port/nanos-port /work/port
	cp "$(HTOP_PORT)/htop.nxe" $(BINFOLDER)htop.nxe
	@echo "staged $(BINFOLDER)htop.nxe — run 'make image64' to install it into /nanos/bin"

# Node.js v24.15.0 — the JavaScript runtime Electron embeds (plan 02). x86_64-ONLY. Cross-builds Node
# + its embedded V8 from $(NODE_SRC) (fetched by scripts/electron/fetch-node.sh) in the nanos-sdk-dev
# container against the x86_64-nanos sysroot, then mknx-converts the ELF to bin/node.nxe. The exact
# configure+make record is scripts/electron/build-node.sh; the port (compiler-flag wrappers, source
# patches, the build log) lives in ports/node/ and docs/en/node-port.md. `make image*` never depends
# on this. NOTE: currently blocked on the SDK libstdc++ threading rebuild (see docs/en/node-port.md).
NODE_SRC     ?= $(SDK_WORK)/node-src
NODE_TRIPLE  := x86_64-nanos
node:
	@test -d "$(NODE_SRC)" || { echo "node source not found at $(NODE_SRC) — run 'bash scripts/electron/fetch-node.sh' first"; exit 1; }
	@test -d "$(SDK_TC)/$(NODE_TRIPLE)/include" || { echo "nanos-sdk $(NODE_TRIPLE) toolchain not found at $(SDK_TC)"; exit 1; }
	$(NXPORT_PREREQ)
	# Refresh the x86_64-nanos sysroot from THIS checkout so the cross build tracks the live ABI
	# (POSIX + Node-specific headers, libc.ndl{,.a}, the crt0/nxhdr startup objects, mknx).
	cp -R user/libc-glue/include/. "$(SDK_TC)/$(NODE_TRIPLE)/include/"
	cp kernel/SyscallNr.h            "$(SDK_TC)/$(NODE_TRIPLE)/include/SyscallNr.h"
	cp user/libc-glue/nx-dllimport.h "$(SDK_TC)/$(NODE_TRIPLE)/include/nx-dllimport.h"
	# nx-node.ld = the default nx.ld + a bracketed .ctors/.init_array collection: node's toolchain
	# emits legacy .ctors, and the stock nx.ld leaves them unbracketed so crt0 runs no C++ static
	# constructor (V8 globals stay null -> #PF in bootstrap). Staged AS the default script (the gcc
	# spec's %{!T:-T nx.ld}) rather than injected via -Wl,-T, because ld resets the collected
	# section's LMA to the base under -T but not as the default script. See ports/node/nx-node.ld.
	cp ports/node/nx-node.ld         "$(SDK_TC)/$(NODE_TRIPLE)/lib/nx.ld"
	cp $(BINFOLDER)libc.ndl.a        "$(SDK_TC)/$(NODE_TRIPLE)/lib/libc.a"
	cp $(BINFOLDER)libc.ndl          "$(SDK_TC)/$(NODE_TRIPLE)/lib/libc.ndl"
	cp $(BINFOLDER)crt0.o            "$(SDK_TC)/$(NODE_TRIPLE)/lib/crt0.o"
	cp $(BINFOLDER)nxhdr.o           "$(SDK_TC)/$(NODE_TRIPLE)/lib/nxhdr.o"
	cp $(BINFOLDER)mknx64            "$(SDK_TC)/bin/$(NODE_TRIPLE)-mknx"
	docker run --rm --memory=14g \
	  -v "$(CURDIR)":/src -v "$(SDK_TC)":/work/toolchain -v "$(NODE_SRC)":/work/node \
	  -e PATH="/work/toolchain/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" -e JOBS=4 \
	  -w /work/node nanos-sdk-dev:latest bash /src/scripts/electron/build-node.sh
	x86_64-nanos-mknx "$(NODE_SRC)/out/Release/node" $(BINFOLDER)node.nxe --need libc.ndl || \
	  docker run --rm -v "$(SDK_TC)":/work/toolchain -v "$(NODE_SRC)":/work/node -v "$(CURDIR)/bin":/out \
	    -e PATH="/work/toolchain/bin:/usr/bin:/bin" nanos-sdk-dev:latest \
	    x86_64-nanos-mknx /work/node/out/Release/node /out/node.nxe --need libc.ndl
	@echo "staged $(BINFOLDER)node.nxe — run 'make image64' to install it into /nanos/bin"

# SQLite 3.46.1 — the real `sqlite3` command-line shell + libsqlite.ndl shared library. x86_64-ONLY.
# Built from the SQLite fork (amalgamation sqlite3.c + the CLI shell.c) via the reproducible
# nx-port-build.sh driver: SQLite has no configure step, so the driver just cross-compiles + links
# the .nxe. nanos/compat.c supplies the advisory-lock no-op fcntl (NanOS has no POSIX byte-range
# locks); fsync/ftruncate are REAL, so a database on the read-write ext4 /disks/main is durable and
# crash-consistent. Produces bin/sqlite3.nxe (-> /nanos/bin) and bin/libsqlite.ndl{,.a} (-> /nanos/lib,
# for apps that want to import the SQL engine by name). `make image*` never depends on this.
SQLITE_FORK ?= $(HOME)/Projects/sqlite-nanos
sqlite:
	@test -f "$(SQLITE_FORK)/sqlite3.c" || { echo "sqlite fork not found at $(SQLITE_FORK) (set SQLITE_FORK=/path/to/sqlite-nanos)"; exit 1; }
	$(NXPORT_PREREQ)
	$(NXPORT_RUN) -v "$(SQLITE_FORK)":/work/src $(DOCKER_IMAGE) sh /src/scripts/nx-port-build.sh sqlite3
	cp "$(SQLITE_FORK)/sqlite3.nxe" $(BINFOLDER)sqlite3.nxe
	@cp "$(SQLITE_FORK)/libsqlite.ndl"   $(BINFOLDER)libsqlite.ndl   2>/dev/null || true
	@cp "$(SQLITE_FORK)/libsqlite.ndl.a" $(BINFOLDER)libsqlite.ndl.a 2>/dev/null || true
	@echo "staged $(BINFOLDER)sqlite3.nxe (+ libsqlite.ndl) — run 'make image64' to install into /nanos/bin (+ /nanos/lib)"

bzip2:
	$(NXPORT_PREREQ)
	$(NXPORT_RUN) -v "$(SDK_WORK)/bzip2-1.0.8":/work/src $(DOCKER_IMAGE) sh /src/scripts/nx-port-build.sh bzip2
	cp "$(SDK_WORK)/bzip2-1.0.8/bzip2.nxe" $(BINFOLDER)bzip2.nxe
	@echo "staged $(BINFOLDER)bzip2.nxe — run 'make image64' to install it into /nanos/bin"

# GNU inetutils ping (optional, external). The plan's real ping: SOCK_RAW/ICMP + getaddrinfo,
# unmodified upstream. Unlike grep/vim (prebuilt by hand), this target is REPRODUCIBLE: it
# refreshes the SDK sysroot from THIS checkout (net/POSIX headers in user/libc-glue/include +
# the freshly built libc.ndl{,.a}) and drives nanos-port inside the nanos-sdk-dev container.
# The manifest (configure flags + cross-compile cache) lives at $(SDK_WORK)/inetutils-port.
# `make image` never depends on this; a missing toolchain/port errors clearly and is skipped.
#
# ARCH-AWARE (mirrors `make bash`/`make grep`): the SAME nxport.toml + nanos-port driver builds
# either arch — only the cross triple changes. For ARCH=x86_64 the port targets the x86_64-nanos
# sysroot, sets NX_HOST=x86_64-nanos (configure --host / CC / mknx) + NX_LP64=1 (the driver
# appends the 64-bit sizeofs over the shared 32-bit config.cache), and the in-tree x86_64
# crt0/nxhdr the toolchain default-links are refreshed too (like `make git`).
NANOS_SDK   ?= $(HOME)/Projects/nanos-sdk
SDK_TC      := $(SDK_WORK)/toolchain
PING_PORT   := $(SDK_WORK)/inetutils-port
PING_TRIPLE  := x86_64-nanos
PING_PORT_ENV = -e NX_HOST=x86_64-nanos -e NX_LP64=1
PING_PREREQ   = $(NXPORT_PREREQ)
# x86_64-nanos-gcc default-links the sysroot crt0.o/nxhdr.o; they must track this checkout.
# Refresh the toolchain's mknx with this checkout's mknx64 too: the SDK-baked x86_64-nanos-mknx
# predates mknx64's Windows-style DATA auto-import (stdin/stdout/stderr/errno from libc.ndl), so
# the port's final mknx would fail "undefined function 'stdout'". mknx64 is the source of truth.
PING_STARTUP  = cp $(BINFOLDER)crt0.o "$(SDK_TC)/$(PING_TRIPLE)/lib/crt0.o"; cp $(BINFOLDER)nxhdr.o "$(SDK_TC)/$(PING_TRIPLE)/lib/nxhdr.o"; cp $(BINFOLDER)mknx64 "$(SDK_TC)/bin/$(PING_TRIPLE)-mknx"
# The honest-conftest gcc wrapper pairs with a per-libc data stub (weak defs of libc.ndl's
# DATA-only exports). Regenerate it from the libc.a we just refreshed, so autoconf link probes
# resolve real functions strictly from libc.a, data from the stub, and absent symbols fail —
# without it, every AC_CHECK_FUNC passes and gnulib misdetects MSVC/getgrouplist/... and breaks.
PING_PRECMD   = NM=$(PING_TRIPLE)-nm CONFTEST_STUB_CC=$(PING_TRIPLE)-gcc.real sh /work/toolchain/bin/gen-conftest-stubs.sh /work/toolchain/$(PING_TRIPLE)/lib &&
# For build=make recipes (darkhttpd): x86_64 objects need the small-model/no-PIC set + the
# data-import shim, spliced into the recipe's CFLAGS as $${NXPORT_EXTRA_CFLAGS} (nanos-port
# defaults it empty).
NXPORT_XCFLAGS = -e NXPORT_EXTRA_CFLAGS="-fno-pie -fno-PIC -mcmodel=small -mno-red-zone -include nx-dllimport.h"
ping: bin/libc.ndl bin/libc.ndl.a
	@test -d "$(SDK_TC)/$(PING_TRIPLE)/include" || { echo "nanos-sdk $(PING_TRIPLE) toolchain not found at $(SDK_TC)"; exit 1; }
	@test -f "$(PING_PORT)/nxport.toml"     || { echo "inetutils port not found at $(PING_PORT)/nxport.toml"; exit 1; }
	# (Re)build the in-tree 64-bit libc/crt0/nxhdr/mknx first.
	$(PING_PREREQ)
	# Refresh the SDK sysroot from this repo (the source of truth for headers + libc).
	cp -R user/libc-glue/include/. "$(SDK_TC)/$(PING_TRIPLE)/include/"
	cp kernel/SyscallNr.h          "$(SDK_TC)/$(PING_TRIPLE)/include/SyscallNr.h"
	# The dllimport shim (stdin/stdout/stderr/environ/_ctype_b -> libc.ndl IAT slots). x86_64
	# code references these DATA exports RIP-relative (R_X86_64_PC32), which mknx can't auto-
	# import; the shim turns each into an __imp_<name> slot deref. The port's post_configure
	# hook #includes it into config.h.
	cp user/libc-glue/nx-dllimport.h "$(SDK_TC)/$(PING_TRIPLE)/include/nx-dllimport.h"
	cp $(BINFOLDER)libc.ndl.a      "$(SDK_TC)/$(PING_TRIPLE)/lib/libc.a"
	cp $(BINFOLDER)libc.ndl        "$(SDK_TC)/$(PING_TRIPLE)/lib/libc.ndl"
	$(PING_STARTUP)
	docker run --rm \
	  -v "$(SDK_TC)":/work/toolchain -v "$(PING_PORT)":/work/port -v "$(NANOS_SDK)":/sdk \
	  -e SDK=/sdk $(PING_PORT_ENV) -e PATH="/work/toolchain/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
	  -w /work/port nanos-sdk-dev:latest sh -c '$(PING_PRECMD) python3 /sdk/port/nanos-port /work/port'
	cp "$(PING_PORT)/ping.nxe" $(BINFOLDER)ping.nxe
	@echo "staged $(BINFOLDER)ping.nxe — run 'make image64' to install it into /nanos/bin"

# GNU wget 1.21.4 (optional, external). Unmodified upstream, HTTP-only (no TLS yet). Same
# reproducible flow as `make ping`: refresh the SDK sysroot from this checkout, drive nanos-port.
# Manifest at $(SDK_WORK)/wget-port. `make image` never depends on this.
WGET_PORT   := $(SDK_WORK)/wget-port
wget: bin/libc.ndl bin/libc.ndl.a
	@test -d "$(SDK_TC)/$(PING_TRIPLE)/include" || { echo "nanos-sdk $(PING_TRIPLE) toolchain not found at $(SDK_TC)"; exit 1; }
	@test -f "$(WGET_PORT)/nxport.toml"     || { echo "wget port not found at $(WGET_PORT)/nxport.toml"; exit 1; }
	@test -f "$(SDK_TC)/$(PING_TRIPLE)/lib/libssl.a" || { echo "libssl.a not in the $(PING_TRIPLE) sysroot — run 'make ARCH=$(ARCH) openssl' first"; exit 1; }
	# ARCH-AWARE via the PING_* variables (same triple/env/prereq set; nxport.toml resolves
	# the openssl sysroot through $${NX_HOST}, which nanos-port defaults + expands).
	$(PING_PREREQ)
	cp -R user/libc-glue/include/. "$(SDK_TC)/$(PING_TRIPLE)/include/"
	cp kernel/SyscallNr.h          "$(SDK_TC)/$(PING_TRIPLE)/include/SyscallNr.h"
	cp user/libc-glue/nx-dllimport.h "$(SDK_TC)/$(PING_TRIPLE)/include/nx-dllimport.h"
	cp $(BINFOLDER)libc.ndl.a      "$(SDK_TC)/$(PING_TRIPLE)/lib/libc.a"
	cp $(BINFOLDER)libc.ndl        "$(SDK_TC)/$(PING_TRIPLE)/lib/libc.ndl"
	$(PING_STARTUP)
	docker run --rm \
	  -v "$(SDK_TC)":/work/toolchain -v "$(WGET_PORT)":/work/port -v "$(NANOS_SDK)":/sdk \
	  -e SDK=/sdk $(PING_PORT_ENV) -e PATH="/work/toolchain/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
	  -w /work/port nanos-sdk-dev:latest sh -c '$(PING_PRECMD) python3 /sdk/port/nanos-port /work/port'
	cp "$(WGET_PORT)/wget.nxe" $(BINFOLDER)wget.nxe
	@echo "staged $(BINFOLDER)wget.nxe — run 'make image64' to install it into /nanos/bin"

# GNU git 2.54.0 (optional, external). Unmodified upstream source built for NanOS via nanos-port's
# build="make" path (git ships its own Makefile; the cross knobs live in a committed config.mak in
# the source tree — see $(GIT_PORT)/git-2.54.0/config.mak). Local-only git (no http/curl/openssl);
# THREADS ON (pack-objects delta search) and run-command's fork both exercised. The single git.nxe
# bundles all builtins (SKIP_DASHED_BUILT_INS). Same reproducible flow as ping/wget. `make image`
# never depends on this. Requires `make build` first (refreshes the pthread-enabled libc.ndl).
GIT_PORT    := $(SDK_WORK)/git-port
git: bin/libc.ndl bin/libc.ndl.a
	@test -d "$(SDK_TC)/$(PING_TRIPLE)/include" || { echo "nanos-sdk $(PING_TRIPLE) toolchain not found at $(SDK_TC)"; exit 1; }
	@test -f "$(GIT_PORT)/nxport.toml"      || { echo "git port not found at $(GIT_PORT)/nxport.toml"; exit 1; }
	@test -f "$(SDK_TC)/$(PING_TRIPLE)/lib/libz.a" || { echo "libz.a not in the $(PING_TRIPLE) sysroot — run 'make ARCH=$(ARCH) zlib' first"; exit 1; }
	# ARCH-AWARE via the PING_* variables; the nxport.toml passes AR/RANLIB/ZLIB_PATH keyed on
	# $${NX_HOST}, and nanos-port's CC=<host>-gcc beats the committed config.mak's i686 CC.
	$(PING_PREREQ)
	cp -R user/libc-glue/include/. "$(SDK_TC)/$(PING_TRIPLE)/include/"
	cp kernel/SyscallNr.h          "$(SDK_TC)/$(PING_TRIPLE)/include/SyscallNr.h"
	cp user/libc-glue/nx-dllimport.h "$(SDK_TC)/$(PING_TRIPLE)/include/nx-dllimport.h"
	cp $(BINFOLDER)libc.ndl.a      "$(SDK_TC)/$(PING_TRIPLE)/lib/libc.a"
	cp $(BINFOLDER)libc.ndl        "$(SDK_TC)/$(PING_TRIPLE)/lib/libc.ndl"
	# Refresh the startup objects too: crt0.o carries the main-thread TLS bootstrap
	# (__nx_init_tls -> set_thread_area), so a stale crt0 leaves the TCB unset and the first
	# errno access faults. The sysroot copy must track this checkout's crt0/nxhdr.
	cp $(BINFOLDER)crt0.o          "$(SDK_TC)/$(PING_TRIPLE)/lib/crt0.o"
	cp $(BINFOLDER)nxhdr.o         "$(SDK_TC)/$(PING_TRIPLE)/lib/nxhdr.o"
	$(PING_STARTUP)
	docker run --rm \
	  -v "$(SDK_TC)":/work/toolchain -v "$(GIT_PORT)":/work/port -v "$(NANOS_SDK)":/sdk \
	  -e SDK=/sdk $(PING_PORT_ENV) $(NXPORT_XCFLAGS) -e PATH="/work/toolchain/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
	  -w /work/port nanos-sdk-dev:latest sh -c '$(PING_PRECMD) python3 /sdk/port/nanos-port /work/port'
	cp "$(GIT_PORT)/git.nxe" $(BINFOLDER)git.nxe
	@echo "staged $(BINFOLDER)git.nxe — run 'make image64' to install it into /nanos/bin (+ /nanos/libexec/git-core)"

# GNU inetutils SERVICES build (FAZA H): the internet super-server inetd (and, as the build
# grows, telnetd + the telnet/ifconfig/traceroute clients) from the SAME inetutils source as
# `make ping`, but configured with servers enabled. Manifest at $(SERVICES_PORT)/nxport.toml;
# hooks/post_build.sh mknx's the extra binaries. Same reproducible flow as ping/wget.
SERVICES_PORT := $(SDK_WORK)/inetutils-services-port
inetd: bin/libc.ndl bin/libc.ndl.a
	@test -d "$(SDK_TC)/$(PING_TRIPLE)/include" || { echo "nanos-sdk $(PING_TRIPLE) toolchain not found at $(SDK_TC)"; exit 1; }
	@test -f "$(SERVICES_PORT)/nxport.toml"  || { echo "inetutils services port not found at $(SERVICES_PORT)/nxport.toml"; exit 1; }
	# ARCH-AWARE via the PING_* variables (same inetutils source family, same triple/env/prereq).
	$(PING_PREREQ)
	cp -R user/libc-glue/include/. "$(SDK_TC)/$(PING_TRIPLE)/include/"
	cp kernel/SyscallNr.h          "$(SDK_TC)/$(PING_TRIPLE)/include/SyscallNr.h"
	cp user/libc-glue/nx-dllimport.h "$(SDK_TC)/$(PING_TRIPLE)/include/nx-dllimport.h"
	cp $(BINFOLDER)libc.ndl.a      "$(SDK_TC)/$(PING_TRIPLE)/lib/libc.a"
	cp $(BINFOLDER)libc.ndl        "$(SDK_TC)/$(PING_TRIPLE)/lib/libc.ndl"
	$(PING_STARTUP)
	docker run --rm \
	  -v "$(SDK_TC)":/work/toolchain -v "$(SERVICES_PORT)":/work/port -v "$(NANOS_SDK)":/sdk \
	  -e SDK=/sdk $(PING_PORT_ENV) -e PATH="/work/toolchain/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
	  -w /work/port nanos-sdk-dev:latest sh -c '$(PING_PRECMD) python3 /sdk/port/nanos-port /work/port'
	cp "$(SERVICES_PORT)/inetd.nxe" $(BINFOLDER)inetd.nxe
	@for b in telnetd telnet ifconfig traceroute; do \
	  test -f "$(SERVICES_PORT)/$$b.nxe" && cp "$(SERVICES_PORT)/$$b.nxe" $(BINFOLDER)$$b.nxe && echo "  staged $$b.nxe" || true; \
	done
	@echo "staged $(BINFOLDER)inetd.nxe (+ telnetd/telnet/ifconfig/traceroute) — run 'make image64' to install"

# darkhttpd (optional, external): single-file HTTP/1.1 static server (FAZA H4). Built by the
# nanos-sdk from $(HTTPD_PORT)/nxport.toml (build=make, -DNO_IPV6). Same reproducible flow.
HTTPD_PORT := $(SDK_WORK)/darkhttpd-port
httpd: bin/libc.ndl bin/libc.ndl.a
	@test -d "$(SDK_TC)/$(PING_TRIPLE)/include" || { echo "nanos-sdk $(PING_TRIPLE) toolchain not found at $(SDK_TC)"; exit 1; }
	@test -f "$(HTTPD_PORT)/nxport.toml"     || { echo "darkhttpd port not found at $(HTTPD_PORT)/nxport.toml"; exit 1; }
	# ARCH-AWARE via the PING_* variables; build=make, so the x86_64 flag set arrives through
	# $(NXPORT_XCFLAGS) -> the recipe's $${NXPORT_EXTRA_CFLAGS} splice (no config.h to shim).
	$(PING_PREREQ)
	cp -R user/libc-glue/include/. "$(SDK_TC)/$(PING_TRIPLE)/include/"
	cp kernel/SyscallNr.h          "$(SDK_TC)/$(PING_TRIPLE)/include/SyscallNr.h"
	cp user/libc-glue/nx-dllimport.h "$(SDK_TC)/$(PING_TRIPLE)/include/nx-dllimport.h"
	cp $(BINFOLDER)libc.ndl.a      "$(SDK_TC)/$(PING_TRIPLE)/lib/libc.a"
	cp $(BINFOLDER)libc.ndl        "$(SDK_TC)/$(PING_TRIPLE)/lib/libc.ndl"
	$(PING_STARTUP)
	docker run --rm \
	  -v "$(SDK_TC)":/work/toolchain -v "$(HTTPD_PORT)":/work/port -v "$(NANOS_SDK)":/sdk \
	  -e SDK=/sdk $(PING_PORT_ENV) $(NXPORT_XCFLAGS) -e PATH="/work/toolchain/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
	  -w /work/port nanos-sdk-dev:latest sh -c '$(PING_PRECMD) python3 /sdk/port/nanos-port /work/port'
	cp "$(HTTPD_PORT)/darkhttpd.nxe" $(BINFOLDER)darkhttpd.nxe
	@echo "staged $(BINFOLDER)darkhttpd.nxe — run 'make image64' to install it into /nanos/bin"

# OpenSSL 3.0.15 (optional, external): libcrypto + libssl + the `openssl` CLI (FAZA 1 of the
# TLS/SSH plan). Built by the nanos-sdk from $(OPENSSL_PORT)/nxport.toml (own Perl Configure via
# hooks/pre_configure.sh, no-asm/no-threads/no-shared, seed=/dev/urandom). post_build installs
# libcrypto.a/libssl.a + headers into the SDK sysroot for the downstream TLS/SSH ports. Same
# reproducible flow as ping/wget/httpd. `make image`/`make image64` never depends on this.
#
# ARCH-AWARE (mirrors `make ping`): for ARCH=x86_64 the port targets the x86_64-nanos sysroot,
# sets NX_HOST=x86_64-nanos (pre_configure picks the custom 64-bit-limb `nanos-x86_64` Configure
# target + the LP64/non-PIC small-model cflags + the nx-dllimport.h data-import shim) and NX_LP64=1.
# The in-tree x86_64 crt0/nxhdr/mknx the toolchain default-links are refreshed first (like ping).

# ---- libdrm 2.4.123 (virtgpu-only, static) — the GL stack's DRM userspace (Task 7) ----
# nanos-port's meson build type hardcodes the legacy nanos-cross.meson, so this port
# drives meson directly via build.sh with an x86_64 cross file (cross-nanos64.ini). Installs
# libdrm.a + headers into the x86_64-nanos sysroot for the Mesa port (Task 8) to consume.
LIBDRM_PORT := $(SDK_WORK)/libdrm-port
libdrm: bin/libc.ndl bin/libc.ndl.a
	@test -f "$(LIBDRM_PORT)/build.sh" || { echo "libdrm port not found at $(LIBDRM_PORT) (fetch libdrm-2.4.123 there)"; exit 1; }
	$(NXPORT_PREREQ)
	cp -R user/libc-glue/include/. "$(SDK_TC)/x86_64-nanos/include/"
	cp kernel/SyscallNr.h            "$(SDK_TC)/x86_64-nanos/include/SyscallNr.h"
	cp user/libc-glue/nx-dllimport.h "$(SDK_TC)/x86_64-nanos/include/nx-dllimport.h"
	cp $(BINFOLDER)libc.ndl.a        "$(SDK_TC)/x86_64-nanos/lib/libc.a"
	cp $(BINFOLDER)libc.ndl          "$(SDK_TC)/x86_64-nanos/lib/libc.ndl"
	docker run --rm \
	  -v "$(SDK_TC)":/work/toolchain -v "$(LIBDRM_PORT)":/work/port -v "$(NANOS_SDK)":/sdk \
	  -e PATH="/work/toolchain/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
	  -w /work/port nanos-sdk-dev:latest sh /work/port/build.sh
	@echo "libdrm.a + headers installed into $(SDK_TC)/x86_64-nanos (for the Mesa port)"

# ---- Mesa 24.2.8 (gallium-virgl + EGL + GLES2 + GBM, static) — the GL stack (Task 8) ----
# Consumes the libdrm sysroot; installs the static .a closure + EGL/GLES2/gbm headers back into the
# x86_64-nanos sysroot for the GL apps (gles2info/glkms) and nwm to link. Drives meson directly via
# build.sh (same reason as libdrm: nanos-port's meson build type predates x86_64).
MESA_PORT := $(SDK_WORK)/mesa-port

# Common docker env for the mesa-port link scripts.
MESA_DOCKER_PATH := /work/toolchain/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

# Stage into the x86_64-nanos sysroot everything the mesa-port link scripts expect: libc headers +
# SyscallNr + dllimport shim, the static libc, and the startup objects + mknx (crt0/nxhdr/mknx are
# what build-gles2info.sh / build-glkms.sh reference by path). Mirrors the libdrm target's staging.
define GLAPP_STAGE
	$(NXPORT_PREREQ)
	cp -R user/libc-glue/include/. "$(SDK_TC)/x86_64-nanos/include/"
	cp kernel/SyscallNr.h            "$(SDK_TC)/x86_64-nanos/include/SyscallNr.h"
	cp user/libc-glue/nx-dllimport.h "$(SDK_TC)/x86_64-nanos/include/nx-dllimport.h"
	cp $(BINFOLDER)libc.ndl.a        "$(SDK_TC)/x86_64-nanos/lib/libc.a"
	cp $(BINFOLDER)libc.ndl          "$(SDK_TC)/x86_64-nanos/lib/libc.ndl"
	cp $(BINFOLDER)crt0.o            "$(SDK_TC)/x86_64-nanos/lib/crt0.o"
	cp $(BINFOLDER)nxhdr.o           "$(SDK_TC)/x86_64-nanos/lib/nxhdr.o"
	cp $(BINFOLDER)mknx64            "$(SDK_TC)/bin/x86_64-nanos-mknx"
endef

# One-time bootstrap for the iris build: compile Mesa's intel_clc HOST tool natively inside
# a derived image (nanos-sdk-dev + LLVM/clang/libclc 18) and stage it at $(MESA_PORT)/tools/.
# iris compiles internal OpenCL kernels at build time; there is no Debian intel-clc package.
mesa-intel-clc:
	@test -f "$(MESA_PORT)/Dockerfile.intel-clc" || { echo "Dockerfile.intel-clc not found in $(MESA_PORT)"; exit 1; }
	docker build -f "$(MESA_PORT)/Dockerfile.intel-clc" -t nanos-sdk-dev-clc:latest "$(MESA_PORT)"
	docker run --rm \
	  -v "$(MESA_PORT)":/work/port \
	  -w /work/port nanos-sdk-dev-clc:latest sh /work/port/build-intel-clc.sh
	@echo "intel_clc staged at $(MESA_PORT)/tools/intel_clc"

# The cross build runs in the -clc image: intel_clc (run at build time for the iris internal
# shaders) needs the image's shared LLVM-18 + libclc data files at runtime.
mesa: bin/libc.ndl bin/libc.ndl.a
	@test -f "$(MESA_PORT)/build.sh" || { echo "mesa port not found at $(MESA_PORT) (fetch mesa-24.2.8 there)"; exit 1; }
	@test -f "$(SDK_TC)/x86_64-nanos/lib/libdrm.a" || { echo "run 'make libdrm' first (Mesa consumes it)"; exit 1; }
	@test -x "$(MESA_PORT)/tools/intel_clc" || { echo "run 'make mesa-intel-clc' first (iris needs the host tool)"; exit 1; }
	$(NXPORT_PREREQ)
	cp -R user/libc-glue/include/. "$(SDK_TC)/x86_64-nanos/include/"
	cp kernel/SyscallNr.h            "$(SDK_TC)/x86_64-nanos/include/SyscallNr.h"
	cp user/libc-glue/nx-dllimport.h "$(SDK_TC)/x86_64-nanos/include/nx-dllimport.h"
	cp $(BINFOLDER)libc.ndl.a        "$(SDK_TC)/x86_64-nanos/lib/libc.a"
	cp $(BINFOLDER)libc.ndl          "$(SDK_TC)/x86_64-nanos/lib/libc.ndl"
	docker run --rm \
	  -v "$(SDK_TC)":/work/toolchain -v "$(MESA_PORT)":/work/port -v "$(NANOS_SDK)":/sdk \
	  -e PATH="$(MESA_DOCKER_PATH)" \
	  -w /work/port nanos-sdk-dev-clc:latest sh /work/port/build.sh
	@echo "Mesa .a closure + EGL/GLES2/gbm headers installed into $(SDK_TC)/x86_64-nanos"

# gles2info.nxe — Task 8 bring-up oracle (surfaceless/fd-direct EGL + GLES2 renderer string). Links
# the Mesa .a closure from the sysroot via build-gles2info.sh in docker.
gles2info: bin/libc.ndl bin/libc.ndl.a
	@test -f "$(MESA_PORT)/build-gles2info.sh" || { echo "build-gles2info.sh not found in $(MESA_PORT)"; exit 1; }
	@test -f "$(SDK_TC)/x86_64-nanos/lib/libEGL.a" || { echo "run 'make mesa' first"; exit 1; }
	$(GLAPP_STAGE)
	cp user/gles2info/gles2info.c        "$(MESA_PORT)/gles2info.c"
	cp user/gles2info/nx_stream_bridge.c "$(MESA_PORT)/nx_stream_bridge.c"
	docker run --rm \
	  -v "$(SDK_TC)":/work/toolchain -v "$(MESA_PORT)":/work/port \
	  -e PATH="$(MESA_DOCKER_PATH)" \
	  -w /work/port nanos-sdk-dev:latest sh /work/port/build-gles2info.sh
	cp "$(MESA_PORT)/gles2info.nxe" $(BINFOLDER)gles2info.nxe
	@echo "gles2info.nxe -> $(BINFOLDER)gles2info.nxe"

# glkms.nxe — Task 9 GBM+EGL+KMS present oracle (GL gradient → scanout via AddFB/SetCrtc). Same
# Mesa link closure as gles2info + libdrm KMS wrappers; sources are canonical in user/glkms/.
glkms: bin/libc.ndl bin/libc.ndl.a
	@test -f "$(MESA_PORT)/build-glkms.sh" || { echo "build-glkms.sh not found in $(MESA_PORT)"; exit 1; }
	@test -f "$(SDK_TC)/x86_64-nanos/lib/libgbm.a" || { echo "run 'make mesa' first"; exit 1; }
	$(GLAPP_STAGE)
	cp user/glkms/glkms.c user/glkms/glkms_init.c user/glkms/glkms_init.h "$(MESA_PORT)/"
	cp user/gles2info/nx_stream_bridge.c "$(MESA_PORT)/nx_stream_bridge.c"
	docker run --rm \
	  -v "$(SDK_TC)":/work/toolchain -v "$(MESA_PORT)":/work/port \
	  -e PATH="$(MESA_DOCKER_PATH)" \
	  -w /work/port nanos-sdk-dev:latest sh /work/port/build-glkms.sh
	cp "$(MESA_PORT)/glkms.nxe" $(BINFOLDER)glkms.nxe
	@echo "glkms.nxe -> $(BINFOLDER)glkms.nxe"

# nwm-gl.nxe — Task 10: the nanowm compositor with the GL ES present backend (nw_compose_gl.c)
# compiled in (-DNWM_GL) and linked against the same Mesa .a closure as glkms. The in-tree nwm.nxe
# stays a pure-CPU program (verify64 untouched); nwm-gl.nxe supersedes it on a GL-capable boot and
# falls back to the CPU compositor at runtime (NWM_NO_GL / no DRM node). The NanOS tree is mounted
# read-only so the link script compiles every nwm TU straight from source (no per-file staging).
nwm-gl: bin/libc.ndl bin/libc.ndl.a
	@test -f "$(MESA_PORT)/build-nwm-gl.sh" || { echo "build-nwm-gl.sh not found in $(MESA_PORT)"; exit 1; }
	@test -f "$(SDK_TC)/x86_64-nanos/lib/libgbm.a" || { echo "run 'make mesa' first"; exit 1; }
	$(GLAPP_STAGE)
	cp arch/x86_64/user-nx.ld "$(MESA_PORT)/user-nx.ld"
	docker run --rm \
	  -v "$(SDK_TC)":/work/toolchain -v "$(MESA_PORT)":/work/port -v "$(CURDIR)":/work/nanos:ro \
	  -e PATH="$(MESA_DOCKER_PATH)" \
	  -w /work/port nanos-sdk-dev:latest sh /work/port/build-nwm-gl.sh
	cp "$(MESA_PORT)/nwm-gl.nxe" $(BINFOLDER)nwm-gl.nxe
	@echo "nwm-gl.nxe -> $(BINFOLDER)nwm-gl.nxe"

# image64-gl — a GL desktop image: a byte copy of image64.img with /nanos/bin/nwm.nxe
# swapped for the Mesa-linked nwm-gl.nxe (Task 10). verify64 keeps using the untouched image64
# (original CPU nwm.nxe); the GL scripts/smoke gate use this one. Cheap: copy + one debugfs write.
# The debugfs write runs INSIDE the nanos-build container ($(DOCKER_RUN)) — like the main image64
# build — so it does not depend on e2fsprogs being on the host PATH (keg-only on macOS).
IMAGE64_GL=disk/image64-gl.img
IMAGE64_GL_PART=$(IMAGE64_GL)?offset=69206016
image64-gl: nwm-gl
	@test -f $(IMAGE64) || { echo "run 'make image64' first"; exit 1; }
	cp $(IMAGE64) $(IMAGE64_GL)
	$(DOCKER_RUN) sh -c 'printf "rm /nanos/bin/nwm.nxe\nwrite $(BINFOLDER)nwm-gl.nxe /nanos/bin/nwm.nxe\nset_inode_field /nanos/bin/nwm.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_GL_PART)"'
	@# Start with EMPTY logs: the base image64.img has usually been booted by QEMU smoke gates,
	@# whose runs append to /nanos/logs — flashing those to the Dell mixes stale QEMU verdicts
	@# (boot #47: "renderer=virgl" + two "no-gbm" in gltest.txt were smoke residue, not Dell
	@# output). debugfs rm of an absent file just warns; the build must not fail on it.
	$(DOCKER_RUN) sh -c 'printf "rm /nanos/logs/i915-boot.txt\nrm /nanos/logs/i915test.txt\nrm /nanos/logs/gltest.txt\nrm /nanos/logs/nwm.txt\n" | debugfs -w "$(IMAGE64_GL_PART)"' || true
	@echo "image64-gl -> $(IMAGE64_GL) (nwm = GPU-native GL compositor, logs wiped)"

OPENSSL_PORT := $(SDK_WORK)/openssl-port
OPENSSL_TRIPLE  := x86_64-nanos
OPENSSL_PORT_ENV = -e NX_HOST=x86_64-nanos -e NX_LP64=1
OPENSSL_PREREQ   = $(NXPORT_PREREQ)
OPENSSL_STARTUP  = cp $(BINFOLDER)crt0.o "$(SDK_TC)/$(OPENSSL_TRIPLE)/lib/crt0.o"; cp $(BINFOLDER)nxhdr.o "$(SDK_TC)/$(OPENSSL_TRIPLE)/lib/nxhdr.o"; cp $(BINFOLDER)mknx64 "$(SDK_TC)/bin/$(OPENSSL_TRIPLE)-mknx"
openssl: bin/libc.ndl bin/libc.ndl.a
	@test -d "$(SDK_TC)/$(OPENSSL_TRIPLE)/include" || { echo "nanos-sdk $(OPENSSL_TRIPLE) toolchain not found at $(SDK_TC)"; exit 1; }
	@test -f "$(OPENSSL_PORT)/nxport.toml"   || { echo "openssl port not found at $(OPENSSL_PORT)/nxport.toml"; exit 1; }
	# (Re)build the in-tree 64-bit libc/crt0/nxhdr/mknx first.
	$(OPENSSL_PREREQ)
	cp -R user/libc-glue/include/. "$(SDK_TC)/$(OPENSSL_TRIPLE)/include/"
	cp kernel/SyscallNr.h          "$(SDK_TC)/$(OPENSSL_TRIPLE)/include/SyscallNr.h"
	# The dllimport shim (stdin/stdout/stderr/environ/_ctype_b -> libc.ndl IAT slots). x86_64
	# code references these DATA exports RIP-relative (R_X86_64_PC32), which mknx can't auto-import;
	# pre_configure -include's it into every TU via CFLAGS (OpenSSL's make build has no autotools
	# config.h to append to).
	cp user/libc-glue/nx-dllimport.h "$(SDK_TC)/$(OPENSSL_TRIPLE)/include/nx-dllimport.h"
	cp $(BINFOLDER)libc.ndl.a      "$(SDK_TC)/$(OPENSSL_TRIPLE)/lib/libc.a"
	cp $(BINFOLDER)libc.ndl        "$(SDK_TC)/$(OPENSSL_TRIPLE)/lib/libc.ndl"
	$(OPENSSL_STARTUP)
	docker run --rm \
	  -v "$(SDK_TC)":/work/toolchain -v "$(OPENSSL_PORT)":/work/port -v "$(NANOS_SDK)":/sdk \
	  -e SDK=/sdk $(OPENSSL_PORT_ENV) -e PATH="/work/toolchain/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
	  -w /work/port nanos-sdk-dev:latest python3 /sdk/port/nanos-port /work/port
	cp "$(OPENSSL_PORT)/openssl.nxe" $(BINFOLDER)openssl.nxe
	@echo "staged $(BINFOLDER)openssl.nxe — run 'make image64' to install it into /nanos/bin"

# Dropbear (optional, external): small SSH-2 server (dropbear) + keygen (dropbearkey) + client
# (dbclient) — FAZA 4/5 of the TLS/SSH plan. Bundles its own crypto, runs as root without privsep.
# Built by the nanos-sdk from $(DROPBEAR_PORT)/nxport.toml. Same reproducible flow. `make image`
# installs them; the host reaches sshd via hostfwd 2222->22.
#
# ARCH-AWARE (mirrors `make openssl`): for ARCH=x86_64 the port targets the x86_64-nanos sysroot,
# sets NX_HOST=x86_64-nanos (configure --host / CC / mknx) + NX_LP64=1 (8-byte long/ptr/size_t in
# the cross-cache). The arch CFLAGS/LDFLAGS are passed in the ENVIRONMENT (the manifest no longer
# pins them as configure args): both arches force non-PIC fixed-base ET_EXEC (-fno-pie -fno-PIC /
# -no-pie); x86_64 adds the LP64 small-model/no-red-zone flags + -include nx-dllimport.h (so the
# stdio/errno DATA exports referenced RIP-relative are redirected through libc.ndl's import table).
# The in-tree x86_64 crt0/nxhdr/mknx the toolchain default-links are refreshed first (like ping).

DROPBEAR_PORT := $(SDK_WORK)/dropbear-port
DROPBEAR_TRIPLE   := x86_64-nanos
DROPBEAR_PORT_ENV  = -e NX_HOST=x86_64-nanos -e NX_LP64=1 \
  -e 'CFLAGS=-Os -fno-pie -fno-PIC -mcmodel=small -mno-red-zone -include nx-dllimport.h' \
  -e 'LDFLAGS=-no-pie'
DROPBEAR_PREREQ    = $(NXPORT_PREREQ)
DROPBEAR_STARTUP   = cp $(BINFOLDER)crt0.o "$(SDK_TC)/$(DROPBEAR_TRIPLE)/lib/crt0.o"; cp $(BINFOLDER)nxhdr.o "$(SDK_TC)/$(DROPBEAR_TRIPLE)/lib/nxhdr.o"; cp $(BINFOLDER)mknx64 "$(SDK_TC)/bin/$(DROPBEAR_TRIPLE)-mknx"
dropbear: bin/libc.ndl bin/libc.ndl.a
	@test -d "$(SDK_TC)/$(DROPBEAR_TRIPLE)/include" || { echo "nanos-sdk $(DROPBEAR_TRIPLE) toolchain not found at $(SDK_TC)"; exit 1; }
	@test -f "$(DROPBEAR_PORT)/nxport.toml"   || { echo "dropbear port not found at $(DROPBEAR_PORT)/nxport.toml"; exit 1; }
	# (Re)build the in-tree 64-bit libc/crt0/nxhdr/mknx first.
	$(DROPBEAR_PREREQ)
	cp -R user/libc-glue/include/. "$(SDK_TC)/$(DROPBEAR_TRIPLE)/include/"
	cp kernel/SyscallNr.h          "$(SDK_TC)/$(DROPBEAR_TRIPLE)/include/SyscallNr.h"
	cp user/libc-glue/nx-dllimport.h "$(SDK_TC)/$(DROPBEAR_TRIPLE)/include/nx-dllimport.h"
	cp $(BINFOLDER)libc.ndl.a      "$(SDK_TC)/$(DROPBEAR_TRIPLE)/lib/libc.a"
	cp $(BINFOLDER)libc.ndl        "$(SDK_TC)/$(DROPBEAR_TRIPLE)/lib/libc.ndl"
	$(DROPBEAR_STARTUP)
	docker run --rm \
	  -v "$(SDK_TC)":/work/toolchain -v "$(DROPBEAR_PORT)":/work/port -v "$(NANOS_SDK)":/sdk \
	  -e SDK=/sdk $(DROPBEAR_PORT_ENV) -e PATH="/work/toolchain/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
	  -w /work/port nanos-sdk-dev:latest python3 /sdk/port/nanos-port /work/port
	cp "$(DROPBEAR_PORT)/dropbear.nxe" $(BINFOLDER)dropbear.nxe
	@for b in dropbearkey dbclient; do \
	  test -f "$(DROPBEAR_PORT)/$$b.nxe" && cp "$(DROPBEAR_PORT)/$$b.nxe" $(BINFOLDER)$$b.nxe && echo "  staged $$b.nxe" || true; \
	done
	@echo "staged $(BINFOLDER)dropbear.nxe (+ dropbearkey/dbclient) — run 'make image64' to install"

# busybox udhcpc (DHCP client, FAZA F). Reproducible like ping/wget: refresh the SDK sysroot from
# this checkout, then cross-build busybox configured with ONLY udhcpc (nanos-build.sh in the
# busybox tree) and mknx it. No busybox source patches — sysroot headers + EXTRA_CFLAGS only.
# The action helper (dhcpcfg.nxe) ships separately as /nanos/config/udhcpc.script via _image64.
BB_DIR := $(SDK_WORK)/busybox-1.36.1
udhcpc: bin/libc.ndl bin/libc.ndl.a
	@test -f "$(BB_DIR)/nanos-build.sh" || { echo "busybox not set up at $(BB_DIR) (extract busybox-1.36.1 + nanos-build.sh)"; exit 1; }
	# ARCH-AWARE via the PING_* variables; nanos-build.sh reads NX_HOST for the cross triple
	# and adds the x86_64 small-model/no-PIC/data-import flags itself.
	$(PING_PREREQ)
	cp -R user/libc-glue/include/. "$(SDK_TC)/$(PING_TRIPLE)/include/"
	cp kernel/SyscallNr.h          "$(SDK_TC)/$(PING_TRIPLE)/include/SyscallNr.h"
	cp user/libc-glue/nx-dllimport.h "$(SDK_TC)/$(PING_TRIPLE)/include/nx-dllimport.h"
	cp $(BINFOLDER)libc.ndl.a      "$(SDK_TC)/$(PING_TRIPLE)/lib/libc.a"
	cp $(BINFOLDER)libc.ndl        "$(SDK_TC)/$(PING_TRIPLE)/lib/libc.ndl"
	$(PING_STARTUP)
	docker run --rm -v "$(SDK_WORK)":/work $(PING_PORT_ENV) \
	  -e PATH="/work/toolchain/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
	  -w /work/busybox-1.36.1 nanos-sdk-dev:latest bash -c '\
	    test -f /work/toolchain/$(PING_TRIPLE)/lib/libm.a || $(PING_TRIPLE)-ar rcs /work/toolchain/$(PING_TRIPLE)/lib/libm.a; \
	    rm -f busybox busybox_unstripped networking/udhcp/built-in.o; \
	    bash nanos-build.sh && $(PING_TRIPLE)-mknx busybox_unstripped udhcpc.nxe --need libc.ndl'
	cp "$(BB_DIR)/udhcpc.nxe" $(BINFOLDER)udhcpc.nxe
	@echo "staged $(BINFOLDER)udhcpc.nxe — run 'make image64' to install it into /nanos/bin"

# zlib 1.3.1 (optional, external): the FIRST library port — a static libz.a + zlib.h/zconf.h
# installed into the SDK sysroot, NOT an app (no .nxe, nothing on the disk image). It is the
# foundational compression dependency for the downstream app/lib ports (doom, netsurf's
# libpng/libcurl, dropbear-with-compression). Built by the nanos-sdk from $(ZLIB_PORT)/nxport.toml:
# zlib ships its own hand-rolled ./configure (not autotools), so hooks/pre_configure.sh runs it with
# the cross CC/AR/RANLIB + --static --prefix=sysroot, build="make" drives the produced Makefile, and
# install="sysroot" makes the driver skip mknx (a library has no app binary); hooks/post_build.sh
# `make install`s libz.a + headers into the sysroot. `make image`/`make image64` never depends on
# this. This target ESTABLISHES the library-port convention the later lib ports (ncurses/libpng/
# libjpeg) follow: same nanos-port driver as the app ports, install="sysroot" instead of a .nxe.
#
# ARCH-AWARE (mirrors `make openssl`/`make ping`): for ARCH=x86_64 the port targets the x86_64-nanos
# sysroot, sets NX_HOST=x86_64-nanos (pre_configure picks the cross triple + the LP64/non-PIC
# small-model/no-red-zone cflags + the nx-dllimport.h data-import shim, force-included so zlib's
# gz*.c errno references become __imp_errno IAT slots when a downstream app links libz.a) and
# NX_LP64=1. The in-tree x86_64 crt0/nxhdr/libc the cross gcc default-links (zlib's configure runs
# link probes) are refreshed first, like ping.
ZLIB_PORT := $(SDK_WORK)/zlib-port
ZLIB_TRIPLE  := x86_64-nanos
ZLIB_PORT_ENV = -e NX_HOST=x86_64-nanos -e NX_LP64=1
ZLIB_PREREQ   = $(NXPORT_PREREQ)
ZLIB_STARTUP  = cp $(BINFOLDER)crt0.o "$(SDK_TC)/$(ZLIB_TRIPLE)/lib/crt0.o"; cp $(BINFOLDER)nxhdr.o "$(SDK_TC)/$(ZLIB_TRIPLE)/lib/nxhdr.o"; cp $(BINFOLDER)mknx64 "$(SDK_TC)/bin/$(ZLIB_TRIPLE)-mknx"
zlib: bin/libc.ndl bin/libc.ndl.a
	@test -d "$(SDK_TC)/$(ZLIB_TRIPLE)/include" || { echo "nanos-sdk $(ZLIB_TRIPLE) toolchain not found at $(SDK_TC)"; exit 1; }
	@test -f "$(ZLIB_PORT)/nxport.toml"   || { echo "zlib port not found at $(ZLIB_PORT)/nxport.toml"; exit 1; }
	# (Re)build the in-tree 64-bit libc/crt0/nxhdr/mknx first.
	$(ZLIB_PREREQ)
	cp -R user/libc-glue/include/. "$(SDK_TC)/$(ZLIB_TRIPLE)/include/"
	cp kernel/SyscallNr.h          "$(SDK_TC)/$(ZLIB_TRIPLE)/include/SyscallNr.h"
	# The dllimport shim (errno/stdin/stdout/stderr -> libc.ndl IAT slots). x86_64 code references
	# these DATA exports RIP-relative (R_X86_64_PC32), which mknx can't auto-import; pre_configure
	# -include's it into every TU via CFLAGS.
	cp user/libc-glue/nx-dllimport.h "$(SDK_TC)/$(ZLIB_TRIPLE)/include/nx-dllimport.h"
	cp $(BINFOLDER)libc.ndl.a      "$(SDK_TC)/$(ZLIB_TRIPLE)/lib/libc.a"
	cp $(BINFOLDER)libc.ndl        "$(SDK_TC)/$(ZLIB_TRIPLE)/lib/libc.ndl"
	$(ZLIB_STARTUP)
	docker run --rm \
	  -v "$(SDK_TC)":/work/toolchain -v "$(ZLIB_PORT)":/work/port -v "$(NANOS_SDK)":/sdk \
	  -e SDK=/sdk $(ZLIB_PORT_ENV) -e PATH="/work/toolchain/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
	  -w /work/port nanos-sdk-dev:latest python3 /sdk/port/nanos-port /work/port
	@echo "installed libz.a + zlib.h/zconf.h into the $(ZLIB_TRIPLE) sysroot ($(SDK_TC)/$(ZLIB_TRIPLE)) — link downstream ports with -lz"

# ncurses 6.4 (optional, external): a static-library port — libncurses.a + libtinfo.a + headers
# (curses.h/ncurses.h/term.h/...) installed into the SDK sysroot, NOT an app (no .nxe, nothing on
# the disk image). It is the terminal-handling dependency that unblocks the vim port. Built by the
# nanos-sdk from $(NCURSES_PORT)/nxport.toml the same way as `make zlib`: install="sysroot" makes
# the driver skip mknx (a library has no app binary), build="autotools" runs ncurses' configure
# (--host + the shared cross config.cache + the port's `cache` answers), and hooks/post_build.sh
# `make install`s the libs+headers into the sysroot. TERMINFO is COMPILED IN via
# --with-fallbacks=xterm-256color (no runtime terminfo DB shipped, as in the original port);
# --disable-db-install ships none. `make image`/`make image64` never depends on this.
#
# ARCH-AWARE (mirrors `make zlib`/`make ping`): for ARCH=x86_64 the port targets the x86_64-nanos
# sysroot, sets NX_HOST=x86_64-nanos + NX_LP64=1, and exports CFLAGS with the NanOS x86_64 user ABI
# (-fno-pie/-mcmodel=small/-mno-red-zone) for the cross-target compiles. The nx-dllimport.h data
# shim is appended to include/ncurses_cfg.h by hooks/post_configure.sh (guarded with USE_BUILD_CC so
# ncurses' host build-tools that compile the fallbacks never see it). The in-tree x86_64
# crt0/nxhdr/libc/mknx (link probes + final lib) are refreshed first, and the honest-conftest data
# stub regenerated (PRECMD) so autoconf's link probes detect functions faithfully.
# unchanged (generic cross gcc, absolute relocs, no shim, no extra CFLAGS).
NCURSES_PORT := $(SDK_WORK)/ncurses-port
NCURSES_TRIPLE := x86_64-nanos
NCURSES_PORT_ENV = -e NX_HOST=x86_64-nanos -e NX_LP64=1 -e CFLAGS="-O2 -fno-pie -mcmodel=small -mno-red-zone"
NCURSES_PREREQ   = $(NXPORT_PREREQ)
NCURSES_STARTUP  = cp $(BINFOLDER)crt0.o "$(SDK_TC)/$(NCURSES_TRIPLE)/lib/crt0.o"; cp $(BINFOLDER)nxhdr.o "$(SDK_TC)/$(NCURSES_TRIPLE)/lib/nxhdr.o"; cp $(BINFOLDER)mknx64 "$(SDK_TC)/bin/$(NCURSES_TRIPLE)-mknx"
NCURSES_PRECMD   = NM=$(NCURSES_TRIPLE)-nm CONFTEST_STUB_CC=$(NCURSES_TRIPLE)-gcc.real sh /work/toolchain/bin/gen-conftest-stubs.sh /work/toolchain/$(NCURSES_TRIPLE)/lib &&
ncurses: bin/libc.ndl bin/libc.ndl.a
	@test -d "$(SDK_TC)/$(NCURSES_TRIPLE)/include" || { echo "nanos-sdk $(NCURSES_TRIPLE) toolchain not found at $(SDK_TC)"; exit 1; }
	@test -f "$(NCURSES_PORT)/nxport.toml" || { echo "ncurses port not found at $(NCURSES_PORT)/nxport.toml"; exit 1; }
	# (Re)build the in-tree 64-bit libc/crt0/nxhdr/mknx first.
	$(NCURSES_PREREQ)
	cp -R user/libc-glue/include/. "$(SDK_TC)/$(NCURSES_TRIPLE)/include/"
	cp kernel/SyscallNr.h          "$(SDK_TC)/$(NCURSES_TRIPLE)/include/SyscallNr.h"
	# The dllimport shim (stdout/stderr/errno -> libc.ndl IAT slots). x86_64 lib objects reference
	# these DATA exports RIP-relative (R_X86_64_PC32); the port's post_configure hook #includes it
	# into ncurses_cfg.h.
	cp user/libc-glue/nx-dllimport.h "$(SDK_TC)/$(NCURSES_TRIPLE)/include/nx-dllimport.h"
	cp $(BINFOLDER)libc.ndl.a      "$(SDK_TC)/$(NCURSES_TRIPLE)/lib/libc.a"
	cp $(BINFOLDER)libc.ndl        "$(SDK_TC)/$(NCURSES_TRIPLE)/lib/libc.ndl"
	$(NCURSES_STARTUP)
	docker run --rm \
	  -v "$(SDK_TC)":/work/toolchain -v "$(NCURSES_PORT)":/work/port -v "$(NANOS_SDK)":/sdk \
	  -e SDK=/sdk $(NCURSES_PORT_ENV) -e PATH="/work/toolchain/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
	  -w /work/port nanos-sdk-dev:latest sh -c '$(NCURSES_PRECMD) python3 /sdk/port/nanos-port /work/port'
	@echo "installed libncurses.a + libtinfo.a + headers into the $(NCURSES_TRIPLE) sysroot ($(SDK_TC)/$(NCURSES_TRIPLE)) — link downstream ports with -lncurses -ltinfo"

# libpng 1.6.43 (optional, external): a static-library port — libpng16.a + headers (png.h /
# pngconf.h / pnglibconf.h) installed into the SDK sysroot, NOT an app (no .nxe, nothing on the disk
# image). It is the PNG image codec dependency that unblocks the doom and netsurf ports. Built by the
# nanos-sdk from $(LIBPNG_PORT)/nxport.toml the same way as `make ncurses`: install="sysroot" makes
# the driver skip mknx (a library has no app binary), build="autotools" runs libpng's configure
# (--host + the shared cross config.cache + the port's flags), and hooks/post_build.sh `make install`s
# libpng16.a + headers into the sysroot. Depends on zlib being in the SAME sysroot first
# (`make [ARCH=x86_64] zlib`): libpng's configure finds -lz + zlib.h on the default cross search path,
# so no --with-zlib-prefix is needed. SSE2 filter intrinsics are disabled (--enable-intel-sse=no in
# the manifest) so libpng uses the portable C path — avoids the QEMU-CPU
# SIMD trap. `make image`/`make image64` never depends on this.
#
# ARCH-AWARE (mirrors `make ncurses`/`make zlib`): for ARCH=x86_64 the port targets the x86_64-nanos
# sysroot, sets NX_HOST=x86_64-nanos + NX_LP64=1, and exports CFLAGS with the NanOS x86_64 user ABI
# (-fno-pie/-mcmodel=small/-mno-red-zone) for the cross-target compiles. The nx-dllimport.h data
# shim is appended to the generated config.h by hooks/post_configure.sh (guarded with USE_BUILD_CC),
# so libpng's RIP-relative reference to picolibc's `stderr` DATA becomes an __imp_stderr IAT slot a
# downstream app's mknx fills. The in-tree x86_64 crt0/nxhdr/libc/mknx (link probes + final lib) are
# refreshed first, and the honest-conftest data stub regenerated (PRECMD) so autoconf's link probes
# (AC_CHECK_LIB(z,...) etc.) detect symbols faithfully.
# relocs, no shim, no extra CFLAGS).
LIBPNG_PORT := $(SDK_WORK)/libpng-port
LIBPNG_TRIPLE := x86_64-nanos
LIBPNG_PORT_ENV = -e NX_HOST=x86_64-nanos -e NX_LP64=1 -e CFLAGS="-O2 -fno-pie -mcmodel=small -mno-red-zone"
LIBPNG_PREREQ   = $(NXPORT_PREREQ)
LIBPNG_STARTUP  = cp $(BINFOLDER)crt0.o "$(SDK_TC)/$(LIBPNG_TRIPLE)/lib/crt0.o"; cp $(BINFOLDER)nxhdr.o "$(SDK_TC)/$(LIBPNG_TRIPLE)/lib/nxhdr.o"; cp $(BINFOLDER)mknx64 "$(SDK_TC)/bin/$(LIBPNG_TRIPLE)-mknx"
LIBPNG_PRECMD   = NM=$(LIBPNG_TRIPLE)-nm CONFTEST_STUB_CC=$(LIBPNG_TRIPLE)-gcc.real sh /work/toolchain/bin/gen-conftest-stubs.sh /work/toolchain/$(LIBPNG_TRIPLE)/lib &&
libpng: bin/libc.ndl bin/libc.ndl.a
	@test -d "$(SDK_TC)/$(LIBPNG_TRIPLE)/include" || { echo "nanos-sdk $(LIBPNG_TRIPLE) toolchain not found at $(SDK_TC)"; exit 1; }
	@test -f "$(SDK_TC)/$(LIBPNG_TRIPLE)/lib/libz.a" || { echo "libz.a not in the $(LIBPNG_TRIPLE) sysroot — run 'make $(if $(filter x86_64,$(ARCH)),ARCH=x86_64 ,)zlib' first"; exit 1; }
	@test -f "$(LIBPNG_PORT)/nxport.toml" || { echo "libpng port not found at $(LIBPNG_PORT)/nxport.toml"; exit 1; }
	# (Re)build the in-tree 64-bit libc/crt0/nxhdr/mknx first.
	$(LIBPNG_PREREQ)
	cp -R user/libc-glue/include/. "$(SDK_TC)/$(LIBPNG_TRIPLE)/include/"
	cp kernel/SyscallNr.h          "$(SDK_TC)/$(LIBPNG_TRIPLE)/include/SyscallNr.h"
	# The dllimport shim (stderr/errno -> libc.ndl IAT slots). x86_64 lib objects reference these DATA
	# exports RIP-relative (R_X86_64_PC32); the port's post_configure hook #includes it into config.h
	cp user/libc-glue/nx-dllimport.h "$(SDK_TC)/$(LIBPNG_TRIPLE)/include/nx-dllimport.h"
	cp $(BINFOLDER)libc.ndl.a      "$(SDK_TC)/$(LIBPNG_TRIPLE)/lib/libc.a"
	cp $(BINFOLDER)libc.ndl        "$(SDK_TC)/$(LIBPNG_TRIPLE)/lib/libc.ndl"
	$(LIBPNG_STARTUP)
	docker run --rm \
	  -v "$(SDK_TC)":/work/toolchain -v "$(LIBPNG_PORT)":/work/port -v "$(NANOS_SDK)":/sdk \
	  -e SDK=/sdk $(LIBPNG_PORT_ENV) -e PATH="/work/toolchain/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
	  -w /work/port nanos-sdk-dev:latest sh -c '$(LIBPNG_PRECMD) python3 /sdk/port/nanos-port /work/port'
	@echo "installed libpng16.a + png.h/pngconf.h/pnglibconf.h into the $(LIBPNG_TRIPLE) sysroot ($(SDK_TC)/$(LIBPNG_TRIPLE)) — link downstream ports with -lpng16 -lz"

# libjpeg-turbo 3.0.3 (optional, external): a static-library port — libjpeg.a + libturbojpeg.a +
# headers (jpeglib.h / jconfig.h / jmorecfg.h / jerror.h / turbojpeg.h) installed into the SDK
# sysroot, NOT an app (no .nxe, nothing on the disk image). It is the JPEG image codec dependency
# that unblocks the NetSurf port (image/jpeg). Built by the nanos-sdk from $(LIBJPEG_PORT)/
# nxport.toml the same way as `make libpng`, except libjpeg-turbo is a CMake project (build="cmake"):
# the driver runs cmake in build-nanos/ with -DCMAKE_TOOLCHAIN_FILE=toolchain-nanos.cmake (now itself
# NX_HOST-aware), install="sysroot" makes the driver skip mknx (a library has no app binary), and
# hooks/post_build.sh `cmake --install`s libjpeg.a + libturbojpeg.a + headers into the per-arch
# sysroot. SIMD is off (-DWITH_SIMD=OFF in the manifest) so the portable C codec is used — matches
# the original port and avoids the hand-written NASM SIMD path that has no NanOS ABI. `make image`/
# `make image64` never depends on this.
#
# ARCH-AWARE (mirrors `make libpng`/`make ncurses`): for ARCH=x86_64 the port targets the x86_64-nanos
# sysroot and sets NX_HOST=x86_64-nanos + NX_LP64=1. Unlike the autotools ports (which pass the ABI
# cflags via -e CFLAGS), the CMake ABI/cflags + the nx-dllimport.h DATA-import shim are injected by
# the NX_HOST-aware cmake toolchain file itself (CMAKE_C_FLAGS_INIT for x86_64), so a downstream app
# linking libjpeg.a binds picolibc's `stderr` (jerror.c's default handlers) through the libc.ndl IAT.
# The in-tree x86_64 crt0/nxhdr/libc/mknx are refreshed first (cmake links the cjpeg/djpeg helper

LIBJPEG_PORT := $(SDK_WORK)/libjpeg-port
LIBJPEG_TRIPLE := x86_64-nanos
LIBJPEG_PORT_ENV = -e NX_HOST=x86_64-nanos -e NX_LP64=1
LIBJPEG_PREREQ   = $(NXPORT_PREREQ)
LIBJPEG_STARTUP  = cp $(BINFOLDER)crt0.o "$(SDK_TC)/$(LIBJPEG_TRIPLE)/lib/crt0.o"; cp $(BINFOLDER)nxhdr.o "$(SDK_TC)/$(LIBJPEG_TRIPLE)/lib/nxhdr.o"; cp $(BINFOLDER)mknx64 "$(SDK_TC)/bin/$(LIBJPEG_TRIPLE)-mknx"
libjpeg: bin/libc.ndl bin/libc.ndl.a
	@test -d "$(SDK_TC)/$(LIBJPEG_TRIPLE)/include" || { echo "nanos-sdk $(LIBJPEG_TRIPLE) toolchain not found at $(SDK_TC)"; exit 1; }
	@test -f "$(LIBJPEG_PORT)/nxport.toml" || { echo "libjpeg port not found at $(LIBJPEG_PORT)/nxport.toml"; exit 1; }
	# (Re)build the in-tree 64-bit libc/crt0/nxhdr/mknx first.
	$(LIBJPEG_PREREQ)
	cp -R user/libc-glue/include/. "$(SDK_TC)/$(LIBJPEG_TRIPLE)/include/"
	cp kernel/SyscallNr.h          "$(SDK_TC)/$(LIBJPEG_TRIPLE)/include/SyscallNr.h"
	# The dllimport shim (stderr/errno -> libc.ndl IAT slots). x86_64 lib objects reference these DATA
	# exports RIP-relative (R_X86_64_PC32); the cmake toolchain file -include's it for x86_64 only.
	cp user/libc-glue/nx-dllimport.h "$(SDK_TC)/$(LIBJPEG_TRIPLE)/include/nx-dllimport.h"
	cp $(BINFOLDER)libc.ndl.a      "$(SDK_TC)/$(LIBJPEG_TRIPLE)/lib/libc.a"
	cp $(BINFOLDER)libc.ndl        "$(SDK_TC)/$(LIBJPEG_TRIPLE)/lib/libc.ndl"
	$(LIBJPEG_STARTUP)
	docker run --rm \
	  -v "$(SDK_TC)":/work/toolchain -v "$(LIBJPEG_PORT)":/work/port -v "$(NANOS_SDK)":/sdk \
	  -e SDK=/sdk $(LIBJPEG_PORT_ENV) -e PATH="/work/toolchain/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
	  -w /work/port nanos-sdk-dev:latest python3 /sdk/port/nanos-port /work/port
	@echo "installed libjpeg.a + libturbojpeg.a + headers into the $(LIBJPEG_TRIPLE) sysroot ($(SDK_TC)/$(LIBJPEG_TRIPLE)) — link downstream ports with -ljpeg"

# NetSurf graphical web browser: the engine libs (libcss/libdom/libhubbub/libcurl/...), the
# NanWM libnsfb surface backend, and the netsurf.nxe app all live in the separate netsurf-nanos
# repo (github.com/NanOS-labs/netsurf-nanos), whose port recipes are ARCH-aware via NX_HOST.
# `make netsurf` refreshes the x86_64-nanos sysroot from this checkout's bin/ (libc.ndl +
# libnw.ndl + headers), rebuilds the whole port stack in the nanos-sdk-dev container, and
# stages netsurf.nxe + its res/ tree into bin/. `make image64` then installs the /apps/netsurf
# bundle (+ /bin/netsurf.nxe symlink). Needs `make openssl` done once (HTTPS). Launch in nwm.
NETSURF_REPO ?= $(HOME)/Projects/netsurf-nanos
netsurf: bin/libc.ndl bin/libc.ndl.a bin/libnw.ndl bin/libnw.ndl.a
	@test -f "$(NETSURF_REPO)/scripts/build-all.sh" || { echo "netsurf-nanos repo not found at $(NETSURF_REPO)"; exit 1; }
	NX_HOST=x86_64-nanos sh "$(NETSURF_REPO)/scripts/sync-sysroot.sh"
	NX_HOST=x86_64-nanos sh "$(NETSURF_REPO)/scripts/build-all.sh"
	cp "$(NETSURF_REPO)/ports/netsurf/netsurf.nxe" $(BINFOLDER)netsurf.nxe
	rm -rf $(BINFOLDER)netsurf-res && cp -R "$(NETSURF_REPO)/ports/netsurf/res" $(BINFOLDER)netsurf-res
	@echo "staged $(BINFOLDER)netsurf.nxe + res — run 'make image64' to install /apps/netsurf"

# Stage EVERY already-built external app into bin/ in one go (best-effort: skips any whose artifact
# is not present, so a partial set still works). The staged .nxe are build artifacts that `make
# clean` removes, so the workflow after a clean is: `make externals && make image`. This copies
# prebuilt binaries only — it does NOT rebuild them (use `make bash`/the nanos-sdk for that).
externals:
	@n=0; \
	for spec in "bash:$(BASH_FORK)/nanos/bash.nxe" \
	            "vim:$(SDK_WORK)/vim/src/vim.nxe" \
	            "grep:$(SDK_WORK)/grep-3.11/grep.nxe" \
	            "bzip2:$(SDK_WORK)/bzip2-1.0.8/bzip2.nxe" \
	            "ping:$(SDK_WORK)/inetutils-port/ping.nxe" \
	            "wget:$(SDK_WORK)/wget-port/wget.nxe" \
	            "inetd:$(SDK_WORK)/inetutils-services-port/inetd.nxe" \
	            "telnetd:$(SDK_WORK)/inetutils-services-port/telnetd.nxe" \
	            "telnet:$(SDK_WORK)/inetutils-services-port/telnet.nxe" \
	            "ifconfig:$(SDK_WORK)/inetutils-services-port/ifconfig.nxe" \
	            "traceroute:$(SDK_WORK)/inetutils-services-port/traceroute.nxe" \
	            "darkhttpd:$(SDK_WORK)/darkhttpd-port/darkhttpd.nxe" \
	            "openssl:$(SDK_WORK)/openssl-port/openssl.nxe" \
	            "udhcpc:$(BB_DIR)/udhcpc.nxe"; do \
	  name=$${spec%%:*}; src=$${spec#*:}; \
	  if [ -f "$$src" ]; then cp "$$src" "$(BINFOLDER)$$name.nxe"; echo "  staged $$name.nxe"; n=$$((n+1)); \
	  else echo "  skip $$name (not built: $$src)"; fi; \
	done; \
	if [ -f "$(NETSURF_REPO)/ports/netsurf/netsurf.nxe" ]; then \
	  cp "$(NETSURF_REPO)/ports/netsurf/netsurf.nxe" $(BINFOLDER)netsurf.nxe; \
	  rm -rf $(BINFOLDER)netsurf-res && cp -R "$(NETSURF_REPO)/ports/netsurf/res" $(BINFOLDER)netsurf-res; \
	  echo "  staged netsurf.nxe + res"; n=$$((n+1)); \
	else echo "  skip netsurf (not built: $(NETSURF_REPO)/ports/netsurf/netsurf.nxe)"; fi; \
	echo "staged $$n external app(s) into $(BINFOLDER) — run 'make image64' to install them"

# Build EVERY external port from source and produce the full bootable image — the whole
# ECOSYSTEM.md recipe in one command. Layers run serially via recursive make because each
# one consumes the previous one's sysroot artifacts (vim/htop need ncurses' libtinfo, wget
# needs openssl, mesa needs libdrm...). mesa-intel-clc is a one-time host-tool bootstrap:
# built only when $(MESA_PORT)/tools/intel_clc is missing (the mesa target hard-requires it).
# Needs the port checkouts at $(SDK_WORK) (nanos-sdk bootstrap.sh) + the bash/sqlite forks
# in ~/Projects. Cold run is hours (toolchain-heavy); warm reruns are mostly incremental.
world: docker-image
	$(MAKE) zlib openssl ncurses
	$(MAKE) toybox sudo grep bzip2
	$(MAKE) ping wget inetd httpd udhcpc dropbear
	$(MAKE) vim htop git sqlite bash
	$(MAKE) doom
	$(MAKE) libpng libjpeg
	@test -x "$(MESA_PORT)/tools/intel_clc" || $(MAKE) mesa-intel-clc
	$(MAKE) libdrm mesa gles2info glkms nwm-gl
	$(MAKE) netsurf
	$(MAKE) externals
	$(MAKE) assets || true
	$(MAKE) image64
	@echo "world: every port built + staged, image64 ready — 'make run64' to boot it"

# Desktop artwork: convert the branded PNGs (NanOS wallpaper + logo) to NanOS's flat 32bpp surface
# format on the host (needs python3 + Pillow), staged into bin/ where _image64 installs them under
# /nanos/share. The compositor uses wallpaper.raw as the desktop background and About shows logo.raw;
# both fall back gracefully if absent. Source PNGs live in assets/ (override with ART_DIR=).
ART_DIR ?= $(CURDIR)/assets
.PHONY: world assets externals bash grep vim bzip2 ping wget git inetd httpd udhcpc zlib ncurses libpng libjpeg htop libdrm mesa mesa-intel-clc gles2info glkms nwm-gl  # never confuse these with the assets/ dir or bin/ files
assets:
	@command -v python3 >/dev/null 2>&1 || { echo "need python3 + Pillow for assets"; exit 1; }
	python3 scripts/png2raw.py "$(ART_DIR)/wallpaper.png" $(BINFOLDER)wallpaper.raw 1024x768 --bg 0x0a1020
	python3 scripts/png2raw.py "$(ART_DIR)/logo.png"      $(BINFOLDER)logo.raw      96x96     --bg 0xf4f8fd
	@echo "staged $(BINFOLDER)wallpaper.raw + logo.raw — run 'make image64' to install them"

# RAM: 512 MiB. The kernel reads the real size from multiboot and lays out its windows above it
# (mmu_x86.cpp), so this is just the QEMU knob — bump it freely (up to ~1 GiB with the current
# window placement). More RAM = bigger kernel heap + a bigger user frame pool.
QEMU_MEM=-m 512

# The x86_64 emulator + CPU for the run64/bringup64 targets (historically pinned separately
# from the per-arch $(QEMU); kept as the single knob the 64-bit run targets use).
QEMU64     ?= qemu-system-x86_64
QEMU_CPU64 ?= -cpu qemu64
# SMP for the interactive run64: 4 vCPUs + MTTCG (each vCPU on its own host thread) so the cores
# run truly in parallel. Override the count with `make run64 NCPU64=N`. (The smoke gates set their
# own flags; this only affects `run64`.)
NCPU64     ?= 4
QEMU_SMP64 ?= -accel tcg,thread=multi -smp $(NCPU64)
# Cocoa display for the interactive run64. QEMU 11.0 made the macOS cocoa backend HiDPI/Retina-aware:
# it now maps 1 guest pixel -> 1 physical pixel, so on a Retina panel the whole window (bootloader
# text included) shows at half physical size and everything looks tiny. `zoom-to-fit=on` restores the
# old behaviour — the guest scales to fill the window, so you can drag/maximise it to a comfortable
# size. Override with QEMU_DISPLAY64= to drop it (e.g. on a non-Retina host or for screendumps).
QEMU_DISPLAY64 ?= -display cocoa,zoom-to-fit=on

# NIC + user-mode NAT + host port-forwards, shared by run64: 5555->80 (httpd), 2323->23
# (telnetd), 5007->7 (echo), 5013->13 (daytime), 5443->5443 (openssl s_server TLS),
# 2222->22 (sshd) — the inetd built-ins + services + the TLS/SSH servers let a host client
# reach the guest servers out of the box (e.g. `telnet localhost 2323`).
NIC_NET=-netdev user,id=n0,hostfwd=tcp::5555-:80,hostfwd=tcp::2323-:23,hostfwd=tcp::5007-:7,hostfwd=tcp::5013-:13,hostfwd=tcp::5443-:5443,hostfwd=tcp::2222-:22 \
        -device e1000,netdev=n0

# Tests run in a lightweight NATIVE-arch image (no amd64 emulation -> fast), since
# they need only g++/lcov, not the cross toolchain or GRUB.
TEST_IMAGE=nanos-test
TEST_DOCKER_RUN=docker run --rm -v $(CURDIR):/src -w /src $(TEST_IMAGE)

test-image:
	docker build -t $(TEST_IMAGE) -f docker/Dockerfile.test docker/

test: test-image
	$(TEST_DOCKER_RUN) make ARCH=$(ARCH) _test

coverage: test-image
	$(TEST_DOCKER_RUN) make ARCH=$(ARCH) _coverage

# --- x86_64 verification gate (Phase 2 of the migration plan) --------------------------------
# `test64` runs the host doctest suite for ARCH=x86_64 — same MI tests as `make test` PLUS the
# 64-bit paging/AddressSpace doctests (test_paging64 / test_addressspace64). Wires the x86_64
# host gate into a single routine command so the 64-bit paging math is checked every run, not
# only when someone remembers to pass ARCH=x86_64.
.PHONY: test64 smoke-x86_64 smoke-uefi smoke-bigmem smoke-e1000e smoke-usb smoke-usb-smp smoke-usb-dmawindow smoke-usb-storm smoke-evidence smoke-fpu smoke-eventfd smoke-mprotect smoke-procself smoke-memfd smoke-shmshare smoke-vt smoke-smp verify64
test64: test-image
	$(TEST_DOCKER_RUN) make ARCH=x86_64 _test

# `i915-probe` scores the i915 compile campaign (Task 5, Dell GPU plan): compile all 276 vendored
# i915 objects against the LinuxKPI shim in Docker and report clean/total + the first-error clusters.
# The in-container `_i915_probe` recipe (+ compile flags) lives in the container section.
i915-probe: docker-image
	$(DOCKER_RUN) make ARCH=x86_64 _i915_probe

# i915 LINK phase host wrappers (in-container recipes live in the x86_64 section):
#   i915-objs           — build all 276 i915 .o (scope link symbols)
#   link-support-probe  — compile-score the TTM/DRM-display/subsystem support objects
i915-objs: docker-image
	$(DOCKER_RUN) make ARCH=x86_64 _i915-objs
link-support-probe: docker-image
	$(DOCKER_RUN) make ARCH=x86_64 _link-support-probe
#   i915-link           — link the full driver to bin/i915.nkext (the Task 5 final milestone)
i915-link: docker-image
	$(DOCKER_RUN) make ARCH=x86_64 bin/i915.nkext

# `smoke-x86_64` is the MACHINE-DEPENDENT half host tests can't reach: build the x86_64 disk image
# and boot it in QEMU headless, asserting the whole MD path came up with zero faults (long-mode,
# GDT/IDT/paging, ATA+ext4 JBD2 write, e1000/net, scheduler, ring-3 fork/exec). Native QEMU on the
# host; the build runs in Docker via the image64 dep.
smoke-x86_64: image64
	bash scripts/smoke-x86_64.sh

# `smoke-usb` is the live-USB boot gate: boot with the whole root filesystem on a USB mass-storage
# device (no -drive disk) and assert the in-kernel xHCI+USB-core+MSC path mounts root and boots.
smoke-usb: image64
	bash scripts/smoke-usb.sh

# `smoke-usb-smp` is the live-USB-on-multicore gate: root-on-USB + -smp 2 (the real Dell's config),
# which exposed the xHCI event-ring data race between USB-MSC reads and the USB-HID poll thread.
# Logs in at the tty7 greeter and requires the nwm desktop to render (a clean read path).
smoke-usb-smp: image64
	bash scripts/smoke-usb-smp.sh

# Concurrent USB-root I/O torture at -smp 4 (the Dell failure mix): CRC-verified parallel readers
# + synchronous appenders + a shared-file append pair + a deliberate crasher. Caught the lost-
# concurrent-append bug (per-fd O_APPEND offset + non-atomic knx_file_append) red-handed.
smoke-usb-storm: image64
	bash scripts/smoke-usb-storm.sh

# Crash-evidence-channel gate: a ring3 SIGSEGV under USB-root storm must leave its [ring3 fault]
# line in i915-boot.txt (kernel-cred append) and the i915 pulse flight recorder must be alive.
smoke-evidence: image64
	bash scripts/smoke-evidence.sh

# FPU/SSE context-switch integrity gate: XMM/MXCSR must survive preemption (per-task FXSAVE).
smoke-fpu: image64
	bash scripts/smoke-fpu.sh

# `smoke-eventfd` is the event-loop-primitive gate (plan 01 Task 1.2 for the Electron program): boot,
# log in, and run the eventfd2 + level-triggered epoll microtests; both must print PASS with no faults.
smoke-eventfd: image64
	bash scripts/smoke-eventfd.sh

# `smoke-mprotect` is the W^X gate (plan 01 Task 1.3): mmapexectest proves the real RW<->RX flip.
smoke-mprotect: image64
	bash scripts/smoke-mprotect.sh

# `smoke-resv` is the reserve-without-backing gate (V8 SegmentedTable): resvtest reserves 128 MiB
# PROT_NONE+MAP_NORESERVE without consuming RAM, commits/decommits segments, and proves uncommitted
# pages fault. This is what unblocks node's JS eval (V8's pointer-table subspace allocation).
smoke-resv: image64
	bash scripts/smoke-resv.sh

# `smoke-tls` is the worker-thread TLS gate: tlstest proves pthread_create builds a real per-thread
# TLS block (__copy_tls) so __thread vars read their initializers + stay private on worker threads —
# what V8/node needs (a worker reading 0 for V8's assert-scope thread_local aborts node).
smoke-tls: image64
	bash scripts/smoke-tls.sh

# `smoke-mallocstorm` is the sbrk-batching gate: mallocstorm times N growing malloc()s against N raw
# getpid() syscalls. A 1:1 sbrk->brk mapping makes each malloc a kernel brk (heavier than getpid) so
# the storm is >= the baseline; batching makes it many times faster. Guards node/V8 startup cost.
smoke-mallocstorm: image64
	bash scripts/smoke-mallocstorm.sh

# `smoke-procself` is the /proc/self gate (plan 01 Task 1.4): exe readlink + fd listing + cwd.
smoke-procself: image64
	bash scripts/smoke-procself.sh

# `smoke-memfd` is the user-writable-tmp + memfd gate (plan 01 Task 1.5 groundwork).
smoke-memfd: image64
	bash scripts/smoke-memfd.sh

# `smoke-shmshare` is the cross-process MAP_SHARED gate (plan 01 Task 1.5 core): shmdualtest proves
# two processes sharing one memfd see each other's writes.
smoke-shmshare: image64
	bash scripts/smoke-shmshare.sh

# Regression gate for the xHCI-DMA-under-user-CR3 fault: rebuilds the kernel with the endpoint ring
# forced into the user-window VA range and asserts root-on-USB boots with no kernel exception (the
# KernelCr3 guard). The script rebuilds a clean image on exit.
smoke-usb-dmawindow:
	bash scripts/smoke-usb-dmawindow.sh

# `smoke-vt` is the virtual-terminal gate: boot, log in on tty1, and drive Ctrl+Alt+Fn via the QEMU
# monitor — switching to tty2 must change the screen, switching back must restore it (byte-exact).
smoke-vt: image64
	bash scripts/smoke-vt.sh

# `smoke-virtio-gpu` is the LinuxKPI virtio-gpu display gate: boot with virtio-gpu as the ONLY
# display (-vga none -device virtio-gpu-pci) and assert the UNMODIFIED Linux 6.12 virtio_gpu DRM
# driver (the full DRM lift on the shim) probes, brings up scanout 0 / bridges /dev/fb0, and
# renders the nwm desktop (no VBE fallback — any pixels prove the driver drives the display).
smoke-virtio-gpu: image64
	bash scripts/smoke-virtio-gpu.sh

# `smoke-i915` is the i915 link+load+bring-up-harness gate (Dell GPU plan Task 5/6): the full
# unmodified Linux 6.12 i915 driver (bin/i915.nkext, 276 objs + full TTM + DRM core + shim) ships
# in the image, gated by /nanos/config/i915. Part A asserts it is a safe no-op unarmed (default
# boot unaffected); Part B arms a throwaway, boots it, and asserts the driver loads, runs i915_init,
# narrates every stage, idles clean on QEMU (no Intel GPU), and that the markers PERSIST to
# /nanos/logs/i915-boot.txt (the Dell hang-survival log channel).
smoke-i915: image64
	bash scripts/smoke-i915.sh

# `smoke-kpi-irq` is the LinuxKPI real-interrupt gate (i915 plan Task 2): boots virtio-gpu as the
# only display and asserts the device's interrupt runs through request_irq -> MSI-X (MsiRouter MSI-X
# + kpi_irq + vt_enable_msi), delivering a real MSI to the handler while the poll stays a watchdog.
smoke-kpi-irq: image64
	bash scripts/smoke-kpi-irq.sh

# `smoke-kpi-wq` is the LinuxKPI async-workqueue gate (i915 plan Task 3): boots virtio-gpu and
# asserts the workqueue subsystem goes inline (pre-scheduler probe) -> async (real worker kthreads)
# without regressing the driver probe / desktop bring-up.
smoke-kpi-wq: image64
	bash scripts/smoke-kpi-wq.sh

# `smoke-virtio-gpu-gl` is the Task-10 GL desktop gate: nwm's GL ES present backend composites the
# desktop and scans it out via GBM+EGL+KMS (glkms) instead of blitting /dev/fb0. It needs the virgl
# fork QEMU AND a macOS windowed GUI (cocoa gl=es / ANGLE→Metal — no headless path), so it is a
# DEVELOPER gate (run on your Mac), NOT part of headless verify64; it SKIPs cleanly without the fork.
smoke-virtio-gpu-gl: image64-gl
	bash scripts/smoke-virtio-gpu-gl.sh

# `bench64-gl` is the automated GUI-rendering benchmark: boots the GL desktop, launches nwbench
# (Cmd+R) in auto mode (NWBENCH_SECS, default 120 s of full-repaint animation), pulls the report
# the app wrote to /users/jan/nwbench-result.txt out of the image and prints it; runs archive to
# scratch/bench/ (latest.txt = most recent). Same developer-gate caveats as smoke-virtio-gpu-gl;
# numbers are A/B-comparable between runs on the same host only (single-core TCG).
bench64-gl: image64-gl
	bash scripts/bench64-gl.sh

# `smoke-sqlite` is the SQLite-port gate: it boots TWICE on the same image and proves the real
# sqlite3 CLI creates a database on the read-write ext /disks/main AND that it survives a reboot
# (3 rows persist; a second insert makes 6). Requires `make ARCH=x86_64 sqlite` + image64 first.
smoke-sqlite:
	bash scripts/smoke-sqlite.sh

# `smoke-uefi` boots the GPT image under edk2/OVMF firmware (UEFI) so BOOTX64.EFI -> Limine -> kernel
# is exercised — the other half of the dual-firmware boot (smoke-x86_64 covers BIOS/SeaBIOS).
smoke-uefi: image64
	bash scripts/smoke-uefi.sh

# `smoke-bigmem` boots with 6 GiB RAM (a usable region above the 4 GiB PCI hole) to prove the 64-bit
# memory map + 2 MiB huge-page kernel identity map — the gap that breaks boot on a >4 GiB real machine.
smoke-bigmem: image64
	bash scripts/smoke-bigmem.sh

# `smoke-e1000e` boots with the 82574L (-device e1000e) so the shared E1000Core + MSI-X + the minimal
# LAPIC are exercised headless — the QEMU-testable stand-in for the I219's MSI-X/NAPI path.
smoke-e1000e: image64
	bash scripts/smoke-e1000e.sh

# `smoke-smp` is the multicore gate: boot with 4 vCPUs and assert ACPI enumerated them + the AP
# trampoline brought every application processor online with no triple fault.
smoke-smp: image64
	bash scripts/smoke-smp.sh

# `smoke-smp-speedup` is the parallel-SPEEDUP gate: under MTTCG, run the same fixed CPU-bound job as
# 1 worker then 4 workers and assert T(1)/T(4) is a real speedup with an identical checksum — i.e.
# user threads actually run in parallel across cores (not just "4 CPUs are online").
smoke-smp-speedup: image64
	bash scripts/smoke-smp-speedup.sh

# `smoke-smp-stress` is the SMP DATA-RACE gate: boots 4 vCPUs under MTTCG and runs user/smptorture,
# which hammers the per-process FD table + a shared pipe + signals + a mutex counter from 8 threads
# at once with deterministic, order-independent oracles. Unlike the boot/ping/usb smokes it actually
# collides on the structures the fine-grained locks protect — the gate for retiring the BKL (Task 15).
smoke-smp-stress: image64
	bash scripts/smoke-smp-stress.sh

# `smoke-smp-netstress` is the SMP NETWORK data-race gate: boots 4 vCPUs under MTTCG and runs
# user/nettorture (threads ping-ponging UDP over loopback), so the RX-softirq + net-timer kernel
# threads collide with socket syscalls on the shared net stack — the gate for the net lock (15e).
smoke-smp-netstress: image64
	bash scripts/smoke-smp-netstress.sh

# `smoke-futexpp` is the SMP futex LOST-WAKEUP gate: at -smp 4, futexpp ping-pongs a token through a
# condition variable (FUTEX_WAIT/FUTEX_WAKE). If a cross-CPU wake can fire on a not-yet-BLOCKED waiter
# the wakeup is lost and one thread parks forever -> no PASS. This is the exact race that hung V8/node
# in JIT mode at -smp>1 (single core worked). Provably-can-fail: revert the BLOCKED-under-g_futexLock
# fix and this deadlocks.
smoke-futexpp: image64
	bash scripts/smoke-futexpp.sh

# `verify64` = the full x86_64 gate: host tests + BIOS + UEFI + big-RAM + e1000e MSI-X + live-USB + SMP smokes.
verify64: test64 smoke-x86_64 smoke-uefi smoke-bigmem smoke-e1000e smoke-usb smoke-usb-smp smoke-usb-dmawindow smoke-usb-storm smoke-evidence smoke-fpu smoke-eventfd smoke-mprotect smoke-resv smoke-tls smoke-mallocstorm smoke-futexpp smoke-procself smoke-memfd smoke-shmshare smoke-vt smoke-virtio-gpu smoke-i915 smoke-kpi-irq smoke-kpi-wq smoke-smp smoke-smp-speedup smoke-smp-stress smoke-smp-netstress
	@echo "x86_64 verify: host tests + BIOS + UEFI + big-RAM + e1000e MSI + live-USB + live-USB+SMP + VT switch + virtio-gpu (unmodified DRM) + i915 (link/load/harness) + SMP boot + SMP speedup + SMP data-race (stress/netstress) gates all passed."

clean:
	$(DOCKER_RUN) make _clean
	-rm -rf coverage/

# Machine-independence guard: the MI layer (init/kernel/mm/fs/lib/drivers) must
# only reach the arch via <arch/...> contracts — never x86 internals. Runs on the
# host (plain grep, no Docker). Fails if any forbidden reference creeps back in.
MI_CHECK_DIRS=init kernel mm fs lib drivers usb
check-arch:
	@if grep -rnE '#include[[:space:]]*"(Gdt|Idt|Interrupt|IOPort|Paging|PagingControl|AddressSpace|Multiboot)[A-Za-z]*\.h"|\bRegisters\b|\bIRQ[0-9]|__asm__|asm[[:space:]]*\(|asm[[:space:]]+volatile|\b(outb|inb|inw)\b' $(MI_CHECK_DIRS); then \
	   echo "FAIL: machine-dependent reference in MI layer (above)"; exit 1; \
	 else echo "OK: MI layer is arch-clean."; fi

# Per-file LP64 truncation check with the x86_64 toolchain. MI files include only the arch
# CONTRACTS (arch/include) + the freestanding <...> headers, so a single MI TU compiles without
# the rest of arch/x86_64 (filled in by Plans 2-6). Usage:  make convcheck FILE=mm/FrameAllocator.cpp
.PHONY: convcheck
convcheck:
	$(DOCKER_RUN) make ARCH=x86_64 _convcheck FILE=$(FILE)

# x86_64 bring-up (Plan 1): build the minimal long-mode boot image + a tiny GRUB rescue
# ISO in the container, then boot it natively. QEMU's -kernel (multiboot1) loader rejects
# ELF64 ("give a 32bit one"), so we boot through GRUB, whose multiboot1 loader DOES accept
# an ELF64 kernel and hands off in 32-bit PM where loader.S switches to long mode.
.PHONY: bringup64
bringup64:
	$(DOCKER_RUN) make ARCH=x86_64 _bringup64
	@echo "Booting bin/nanos64.iso — expect the staged banner ('NanOS x86_64 -- staged bring-up') on the VGA console."
	$(QEMU64) $(QEMU_CPU64) $(QEMU_MEM) -cdrom $(BINFOLDER)nanos64.iso

# x86_64 staged DISK image (Plan 5): build the staged long-mode kernel (now with the MI storage
# stack linked in) and install it into a hybrid GPT+Limine disk image (BIOS+UEFI) as
# /nanos/core/kernel64.bin, which Limine multiboots. Unlike bringup64 (rescue ISO) this boots from a
# real -drive disk whose ext4 partition the staged kernel mounts at /disks/main and reads/writes.
.PHONY: image64
image64:
	$(DOCKER_RUN) make ARCH=x86_64 _image64

# flash-dell — build the x86_64 disk image and write it to the Kingston boot stick.
# scripts/flash-usb.sh auto-locates the removable USB disk whose media name matches
# "Kingston|DataTraveler" (the Dell Latitude's DataTraveler 3.0), refuses to touch a
# fixed/internal disk, and asks for a typed "yes" before erasing it. Override the match
# with USB_NAME=... , the image with IMAGE=... , or skip the prompt with FORCE=1.
.PHONY: flash-dell
flash-dell: image64
	IMAGE=$(IMAGE64) FORCE=1 ./scripts/flash-usb.sh

# arm-i915 / disarm-i915 — flip the i915 debug harness on the Kingston stick in-place (no
# rebuild, no reflash): writes '1'/'0' to /nanos/config/i915 on the stick's ext partition
# with host debugfs. Needs sudo (raw partition access). See scripts/arm-i915.sh.
.PHONY: arm-i915 disarm-i915
arm-i915:
	VALUE=1 ./scripts/arm-i915.sh
disarm-i915:
	VALUE=0 ./scripts/arm-i915.sh

# i915-inject — dial-a-stop for the probe: write stage N to /nanos/config/i915_inject so i915
# aborts probe cleanly (its own -ENODEV unwind) at its Nth internal injection point. Use after a
# full-probe crash to walk the stop back to the last clean stage — no rebuild/reflash. `make
# i915-inject N=3`; `make i915-inject-off` clears it (full probe).
.PHONY: i915-inject i915-inject-off
i915-inject:
	@[ -n "$(N)" ] || { echo "usage: make i915-inject N=<stage>"; exit 1; }
	KNOB=/nanos/config/i915_inject VALUE=$(N) ./scripts/arm-i915.sh
i915-inject-off:
	KNOB=/nanos/config/i915_inject VALUE=0 ./scripts/arm-i915.sh

# i915-log — after a Dell boot, dump the persisted harness log from the stick
# (/nanos/logs/i915-boot.txt). Bring the Kingston back, run this. Needs sudo.
.PHONY: i915-log
i915-log:
	./scripts/i915-log.sh

# flash-dell-armed — the one-shot for a real Dell i915 run: flash the LATEST image (kernel with
# the 64-bit BAR fix + current i915 kext), THEN arm the knob. Order matters — flashing rewrites
# the whole partition and would wipe a knob written earlier, so arm must come AFTER the flash.
.PHONY: flash-dell-armed
flash-dell-armed: image64
	IMAGE=$(IMAGE64) FORCE=1 NO_EJECT=1 ./scripts/flash-usb.sh
	VALUE=1 ./scripts/arm-i915.sh

# flash-dell-gl — the GL DESKTOP on the Dell: flash the image64-gl variant (nwm.nxe = the
# Mesa-linked GPU compositor) and arm i915 in DESKTOP mode ('2'): driver up with the narrated
# probe, then drm debug off and NO auto-test harness — glkms would steal the compositor's
# scanout mid-session, and per-ioctl drm_dbg at frame rate would grow i915-boot.txt without
# bound. Boot flow on the Dell: fbcon -> greeter (tty7, fb0) -> login -> nwm-gl via KMS.
.PHONY: flash-dell-gl
flash-dell-gl: image64
	$(MAKE) image64-gl
	IMAGE=$(IMAGE64_GL) FORCE=1 NO_EJECT=1 ./scripts/flash-usb.sh
	VALUE=2 ./scripts/arm-i915.sh

# update-dell — the FAST i915 inner loop: rebuild, then push ONLY the changed kernel + i915 kext
# onto the stick with host debugfs (~6 MiB) and re-arm, skipping the ~320 MiB whole-disk dd that
# flash-dell-armed does. Both files live on the ext root, the same partition the image build itself
# populates the same way — so this is byte-identical to a reflash for those two files, just far
# faster. Use it whenever only kernel/i915 code changed (the usual case); use flash-dell-armed after
# a partition-layout change or a fresh stick. `make update-dell` -> boot Dell -> `make i915-log`.
.PHONY: update-dell
update-dell: image64
	KERNEL=$(BINFOLDER)k64/kernel.bin KEXT=$(BINFOLDER)i915.nkext ARM=1 ./scripts/update-dell.sh

# === pendrak — network-attached USB boot device (Pi Zero W presents image64.img to the Dell
# as a mass-storage gadget; push updates over WiFi, no physical stick swap). See
# docs/superpowers/plans/2026-07-05-pi-usb-gadget-dell-boot.md ===
PI_HOST ?= pi@pendrak.local

# pi-bootstrap — one-time idempotent setup of the pendrak Pi (dwc2 peripheral mode, helpers,
# sudoers, systemd gadget unit). Needs passwordless sudo for 'pi' and one reboot after first run.
.PHONY: pi-bootstrap
pi-bootstrap:
	PI_HOST=$(PI_HOST) ./scripts/pi-bootstrap.sh

# flash-dell-pi — full image push: rsync the whole image64.img onto the pendrak Pi (rare;
# use after a partition-layout change or fresh setup). Mirrors flash-dell-armed.
.PHONY: flash-dell-pi
flash-dell-pi: image64
	PI_HOST=$(PI_HOST) IMG=$(IMAGE64) ./scripts/pi-flash.sh

# kernel-kext — build ONLY the x86_64 kernel + kexts (no ~320 MiB image regen). Feeds update-dell-pi
# so the fast loop skips the redundant full-image rebuild that update-dell forces via its image64 dep.
.PHONY: kernel-kext
kernel-kext:
	$(DOCKER_RUN) make ARCH=x86_64 _all _kext

# update-dell-pi — FAST loop over the network: push ONLY kernel + i915 kext to the pendrak Pi and
# run the surgical apply there. Mirrors update-dell. `make update-dell-pi` -> boot Dell -> `make i915-log-pi`.
.PHONY: update-dell-pi
update-dell-pi: kernel-kext
	PI_HOST=$(PI_HOST) KERNEL=$(BINFOLDER)k64/kernel.bin KEXT=$(BINFOLDER)i915.nkext ARM=1 ./scripts/pi-update.sh

# pull-dell-pi / pull-files-pi / i915-log-pi — retrieve data NanOS wrote on the Dell out of the Pi
# backing image. Run ONLY while the Dell is OFF (its writes must be flushed; a live two-writer
# view would corrupt the fs). pull-files-pi PATHS='/a /b', DEST=dir. pull-dell-pi DEST=file.
.PHONY: pull-dell-pi pull-files-pi i915-log-pi
pull-dell-pi:
	PI_HOST=$(PI_HOST) MODE=image DEST=$(or $(DEST),disk/image64-dell.img) ./scripts/pi-pull.sh
pull-files-pi:
	PI_HOST=$(PI_HOST) MODE=files PATHS='$(or $(PATHS),/nanos/logs)' DEST=$(or $(DEST),pull-dell) ./scripts/pi-pull.sh
i915-log-pi:
	PI_HOST=$(PI_HOST) MODE=files PATHS=/nanos/logs/i915-boot.txt DEST=$(BINFOLDER)pull ./scripts/pi-pull.sh
	@echo "----- /nanos/logs/i915-boot.txt -----"; cat $(BINFOLDER)pull/nanos/logs/i915-boot.txt 2>/dev/null || echo "(no i915 log on the image yet)"

run64: image64
	$(QEMU64) $(QEMU_CPU64) $(QEMU_SMP64) $(QEMU_MEM) -drive file=$(IMAGE64),format=raw $(QEMU_DISPLAY64) $(NIC_NET)

# GL/virgl interactive run — like `run64`, but with GPU-accelerated OpenGL ES. Stock homebrew QEMU
# has no virgl, so `run64` cannot show GL; this points at the kosmickrisp fork (virtio-gpu-gl →
# virglrenderer → ANGLE → Metal on Apple Silicon) and adds the cocoa GL display. Boot, log in
# root/nanos (or jan/jan on the graphics VT F7), and the nwm desktop renders GPU-accelerated.
# Override the binary path with QEMU_GL=/path/to/qemu-system-x86_64 if the fork moved. The fork is
# validated single-vCPU (MTTCG-SMP + virgl is flaky), so this pins -smp 1 regardless of NCPU64.
# Default = the from-source qemu-nanos build (qemu-fork-build): same brew virglrenderer/libepoxy
# stack as the old kosmickrisp prefix binary, PLUS the slirp 'user' netdev — so the GL desktop
# gets the same network as run64. The slirp-less kosmickrisp binary still works via
# QEMU_GL=.../qemu-virgl-kosmickrisp/bin/qemu-system-x86_64 QEMU_GL_NET=''.
QEMU_GL     ?= $(HOME)/Projects/nanos-sdk-work/qemu-fork-build/qemu-src/build/qemu-system-x86_64
# Use the PURE virtio-gpu-gl device, NOT virtio-vga-gl: virtio-vga-gl also exposes a legacy VGA
# output that QEMU displays by DEFAULT, so the cocoa gl=es window shows the (black, unused) VGA
# surface and the virtio-gpu scanout — the actual desktop — never appears. virtio-gpu-gl-pci has no
# VGA part, so the virtio-gpu scanout IS the display and the desktop shows. (A/B-proven: with
# virtio-vga-gl the window is black; with virtio-gpu-gl-pci the full nwm desktop composites.)
# `-vga none` is REQUIRED for the GL scanout to reach cocoa: without it QEMU still adds a default
# `-vga std` device, which claims graphic console idx 0 while virtio-gpu-gl-pci scans out on idx 1;
# cocoa binds idx 0, so dpy_gl_scanout_texture is dropped (con != listener con) and the 3D frame
# never displays. `-vga none` makes the GPU console idx 0 and the unmodified dispatch path fires.
# (Isolated-window screencapture A/B-proven with the from-source fork; see plan-1 STATUS, 2026-07-03.)
QEMU_GL_VGA ?= -device virtio-gpu-gl-pci -vga none -display cocoa,gl=es,zoom-to-fit=on
# Same user-mode network as run64 (slirp + the hostfwd set). The default QEMU_GL above has slirp
# compiled in; a slirp-less binary (the old kosmickrisp prefix build) aborts on -netdev user
# ("network backend 'user' is not compiled into this binary") — run it with QEMU_GL_NET=''.
QEMU_GL_NET ?= $(NIC_NET)
.PHONY: run64-gl
run64-gl: image64
	@test -x "$(QEMU_GL)" || { echo "run64-gl: no virgl QEMU at $(QEMU_GL) — build the kosmickrisp fork or set QEMU_GL=..."; exit 1; }
	@# Ad-hoc re-sign the Homebrew tap dylibs the fork dlopen()s (libepoxy/angle/virglrenderer):
	@# bottle relocation invalidates their code signatures and QEMU aborts on a bad one.
	@command -v brew >/dev/null 2>&1 && for keg in libepoxy angle virglrenderer; do \
	  d="$$(brew --prefix startergo/$$keg/$$keg 2>/dev/null)/lib"; \
	  [ -d "$$d" ] && for l in "$$d"/*.dylib; do codesign --force --sign - "$$l" >/dev/null 2>&1; done; \
	done; true
	$(QEMU_GL) $(QEMU_CPU64) -accel tcg,thread=multi -smp 1 $(QEMU_MEM) \
	    -drive file=$(IMAGE64),format=raw $(QEMU_GL_VGA) -no-reboot $(QEMU_GL_NET)

# run64-gl-desktop — boot the GPU-NATIVE desktop (image64-gl = nwm-gl.nxe) on the virgl fork, so the
# WHOLE compositor runs on the GPU: window textures + the two-pass Gaussian GLASS BLUR as a real GL
# shader, scanned out via GBM+EGL+KMS. This differs from `run64-gl`, which boots the plain image64
# (CPU compositor, software blur, shown through the 2D scanout). Same QEMU config (QEMU_GL_VGA =
# virtio-gpu-gl-pci + -vga none + zoom-to-fit, -smp 1) — only the image differs. After boot: switch
# to the graphical VT (Ctrl+Alt+F7) and log in (e.g. jan/jan); apps take a moment to paint under TCG.
# Serial: `nw_gl: GPU compositor init ok` + `nwm: GL compositor active`. Knobs (boot env): NWM_NO_GLASS=1
# opaque, NWM_GL_TRACE=1 blur brackets.
.PHONY: run64-gl-desktop
run64-gl-desktop: image64-gl
	@test -x "$(QEMU_GL)" || { echo "run64-gl-desktop: no virgl QEMU at $(QEMU_GL) — build the kosmickrisp fork or set QEMU_GL=..."; exit 1; }
	@command -v brew >/dev/null 2>&1 && for keg in libepoxy angle virglrenderer; do \
	  d="$$(brew --prefix startergo/$$keg/$$keg 2>/dev/null)/lib"; \
	  [ -d "$$d" ] && for l in "$$d"/*.dylib; do codesign --force --sign - "$$l" >/dev/null 2>&1; done; \
	done; true
	$(QEMU_GL) $(QEMU_CPU64) -accel tcg,thread=multi -smp 1 $(QEMU_MEM) \
	    -drive file=$(IMAGE64_GL),format=raw $(QEMU_GL_VGA) -no-reboot $(QEMU_GL_NET)

# Headless GL proof — boot on the virgl fork, log in, run `gles2info`, print the GL markers to this
# terminal (no cocoa window, no clicking). Success = `renderer=virgl`. The GL-composited desktop is
# a separate unbuilt milestone; this proves only the unmodified-Mesa → virgl → ANGLE → Metal path.
.PHONY: run64-gl-test
run64-gl-test: image64
	QEMU_GL="$(QEMU_GL)" IMG="$(IMAGE64)" bash scripts/run64-gl-selftest.sh

# Doom (in-tree doomgeneric) host wrapper. Stages bin/doom.nxe in the container (doom is NOT
# in the minimal X64_USER_PROGS subset); `make image64` then installs the /apps/doom bundle +
# /bin/doom.nxe symlink.
.PHONY: doom
doom:
	$(DOCKER_RUN) sh -c 'make -j"$$(nproc)" ARCH=$(ARCH) doom'
	@echo "staged $(BINFOLDER)doom.nxe — run 'make image64' to install /apps/doom"

# x86_64 minimal userland (Plan 6): build the 64-bit init.nxe in the container (crt0 +
# nxhdr + libnanos + init, linked at 0x800000, then mknx64 -> v4 .nxe). Host-side wrapper.
.PHONY: init64
init64:
	$(DOCKER_RUN) make ARCH=x86_64 bin/init.nxe

# Plan 10 Tasks 5-6: the x86_64 libc + pthread/TLS ring-3 smokes. Builds a real picolibc program
# (printf) and a pthread/TLS program (errno + a thread) with the x86_64-nanos SDK toolchain
# against the SDK sysroot, proving the 4-artifact libc contract (libc.ndl + import lib + crt0 +
# nxhdr) and the pthread arch port run in ring 3. To run one: install it as PID 1 (overwrite
# /nanos/core/init.nxe in the image) and boot. The sysroot-inject below mirrors the
# `make <app>` cp lines (NANOS_TRIPLE = x86_64-nanos).
NANOS_TRIPLE := x86_64-nanos
SDK_SYSROOT  := $(SDK_TC)/$(NANOS_TRIPLE)
SDK_DEV_IMG  ?= nanos-sdk-dev:latest
SMOKE_PROGS  := hello pthread_hello
.PHONY: smoke64
smoke64:
ifneq ($(ARCH),x86_64)
	$(MAKE) ARCH=x86_64 smoke64
else
	# 1) (Re)build the 64-bit libc artifacts in the kernel-toolchain container (ELF64 libc.ndl +
	#    import lib + the TLS-bootstrap crt0 + nxhdr). `make clean` first avoids mixing a stale
	#    stale bin/*.o of another vintage into the link (libc-glue objects are not suffixed).
	$(DOCKER_RUN) sh -c 'make ARCH=x86_64 bin/libc.ndl bin/libc.ndl.a bin/crt0.o bin/nxhdr.o'
	# 2) Inject them + the LP64 porting headers + the x86_64 SyscallNr.h into the SDK sysroot.
	@test -d "$(SDK_SYSROOT)/include" || { echo "nanos-sdk toolchain not found at $(SDK_SYSROOT)"; exit 1; }
	cp -R user/libc-glue/include/. "$(SDK_SYSROOT)/include/"
	cp kernel/SyscallNr.h          "$(SDK_SYSROOT)/include/SyscallNr.h"
	cp $(BINFOLDER)libc.ndl.a      "$(SDK_SYSROOT)/lib/libc.a"
	cp $(BINFOLDER)libc.ndl        "$(SDK_SYSROOT)/lib/libc.ndl"
	cp $(BINFOLDER)crt0.o          "$(SDK_SYSROOT)/lib/crt0.o"
	cp $(BINFOLDER)nxhdr.o         "$(SDK_SYSROOT)/lib/nxhdr.o"
	# 3) Build each smoke .nxe with the SDK gcc: compile against the picolibc sysroot
	#    (nx-dllimport.h redirects the stdio DATA imports), link the import lib + crt0/nxhdr at
	#    the .ndl base, then mknx into a v4 .nxe that imports libc.ndl by name.
	docker run --rm -v "$(SDK_TC)":/work/toolchain -v "$(CURDIR)":/src \
	  -e PATH="/work/toolchain/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
	  -w /src $(SDK_DEV_IMG) sh -c 'set -e; SR=/work/toolchain/$(NANOS_TRIPLE); \
	    for s in $(SMOKE_PROGS); do \
	      $(NANOS_TRIPLE)-gcc -O2 -ffreestanding -include user/libc-glue/nx-dllimport.h -c user/smoke/$$s.c -o $(BINFOLDER)$$s.o; \
	      $(NANOS_TRIPLE)-gcc -nostdlib -Wl,--emit-relocs -T $$SR/lib/nx.ld -o $(BINFOLDER)$$s.elf $$SR/lib/crt0.o $$SR/lib/nxhdr.o $(BINFOLDER)$$s.o $$SR/lib/libc.a -lgcc; \
	      $(NANOS_TRIPLE)-mknx $(BINFOLDER)$$s.elf $(BINFOLDER)$$s.nxe --need libc.ndl; \
	    done'
	@echo "built $(addprefix $(BINFOLDER),$(addsuffix .nxe,$(SMOKE_PROGS))) — install one over /nanos/core/init.nxe (+ bin/libc.ndl to /nanos/lib) to run under QEMU"
endif

else
# ============================================================================
# CONTAINER side (Linux): real compilation, image and ISO creation.
# Tools resolved from the container PATH (x86_64-elf toolchain in /opt/cross).
# ============================================================================

CXX=$(CROSS)gcc
AS=$(CROSS)as
LD=$(CROSS)gcc

# Sources live in layered directories; let make find them by basename. The MI
# dirs are fixed; the arch's dirs come from ARCH_VPATH (arch/$(ARCH)/...).
VPATH=init:kernel:kernel/vt:drivers:fs:fs/ext:mm:lib:net:usb:kext/mouse:$(ARCH_VPATH)
# Kernel include path: the MI code dirs + the arch contracts (arch/include) + the
# selected arch's headers (ARCH_INCLUDES) + the freestanding <string.h> in include/.
KINCLUDES=-Iarch/include -Iinit -Ikernel -Idrivers -Ifs -Imm -Ilib -Inet -Iusb -Ikext/mouse -Ikext/virtio_gpu -Iinclude -Iuser/term $(ARCH_INCLUDES)

# Optimization. -O2 is the single biggest performance win for the target (it was all -O0): pixel
# loops get inlined and the ~10x interpreter-style overhead disappears. Two safety flags make -O2
# sound here:
#   -fno-strict-aliasing            code casts pointers freely (Registers*, page tables, pixel
#                                   buffers, NXE headers); type-based aliasing would miscompile it.
#   -fno-delete-null-pointer-checks don't assume a dereferenced pointer is non-null (low/identity-
#                                   mapped addresses are real here).
# Not -O3: keeps NXE/kernel size + behaviour predictable.
# Host tests build with their own flags (HOST_CXXFLAGS), unaffected.
#
# USERLAND gets -O2 now: the compositor's hot pixel loops (nw_gfx/nw_compose blit, blend, glyphs)
# all run in userland writing the mmapped framebuffer directly, so this captures the drag-perf win.
UOPTFLAGS=-O2 -fno-strict-aliasing -fno-delete-null-pointer-checks
# KERNEL is held at -O0 for now: at -O2 a latent undefined behaviour in the ext path-resolution
# string scan (a basename/dirname loop reads past a non-NUL-terminated String buffer) runs off into
# unmapped memory and triple-faults during the boot-time root mount. That UB needs fixing in the FS
# layer before the kernel can take -O2; it is independent of the GUI work, where no kernel code is hot.
KOPTFLAGS=

CXXFLAGS=-ffreestanding -nostdlib -nostdinc++ $(KINCLUDES) -Wall --no-exceptions --no-rtti -fno-sized-deallocation -fno-leading-underscore $(KARCHFLAGS) $(KWFLAGS) $(KOPTFLAGS) $(KEXTRA)
LDFLAGS=-T$(ARCH_LINKER) -nostdlib -nostartfiles -lgcc
ASFLAGS=

# The repo is bind-mounted from a case-insensitive macOS FS, so <string.h> would
# resolve to lib/String.h (infinite include recursion). Compile from a copy on the
# container's own case-sensitive FS, then copy the kernel back.
KSRC=/tmp/nanos-ksrc

# Kernel objects live in their own subdirectory so kernel and userland objects of the same
# objects of the same basename (Kernel.o, Exec.o, ...) never collide in bin/. Switching ARCH
# without a clean would otherwise mix ELF classes (stale elf32 objects linked into an elf64
# kernel → mangling/format mismatches). Kernel objects build into bin/k64/.
# The final kernel.bin still lands at bin/kernel.bin (only one arch is current at a time).
KOBJ=$(BINFOLDER)k64/

# Full link set as paths under the per-arch object dir.
OBJECTS=$(addprefix $(KOBJ),$(SOURCES))

# Sources are compiled from a case-sensitive copy in $(KSRC) (the macOS bind mount is
# case-insensitive, where <string.h> would collide with lib/String.h). But the OBJECTS
# are written into the bind-mounted /src/bin via a symlink, so they PERSIST across
# container runs. tar preserves source mtimes, so make rebuilds only what changed
# (and, via -MMD dep files, what a changed header reaches) instead of everything.
_all:
	@mkdir -p $(BINFOLDER) $(KOBJ)
	@rm -rf $(KSRC) && mkdir -p $(KSRC) && \
	 tar -cf - --exclude=.git --exclude=disk --exclude=bin --exclude=iso --exclude=coverage --exclude=tests -C /src . | tar -xf - -C $(KSRC) && \
	 ln -s /src/$(BINFOLDER) $(KSRC)/bin
	$(MAKE) -C $(KSRC) _compile

_compile: $(KOBJ)kernel.bin

# Syntax-only -Wconversion check of ONE MI source, compiled from a case-sensitive copy (the
# macOS bind mount collides string.h/String.h). Reports warnings for $(FILE) only; sibling
# headers' own warnings are addressed by their own Plan-7 tasks. The final gate (KWFLAGS) is
# what turns these into hard errors across the whole build.
_convcheck:
	@rm -rf $(KSRC) && mkdir -p $(KSRC) && \
	 tar -cf - --exclude=.git --exclude=disk --exclude=bin --exclude=iso --exclude=coverage -C /src . | tar -xf - -C $(KSRC)
	cd $(KSRC) && $(CXX) -fsyntax-only -Wconversion $(CXXFLAGS) $(FILE)

# -lgcc trails the objects so libgcc helper routines first referenced by an object resolve.
# (LDFLAGS also lists -lgcc, but a library only satisfies symbols undefined to its left.)
# kernel.bin lands in $(KOBJ) (bin/k64/kernel.bin) alongside the kernel objects.
$(KOBJ)kernel.bin: $(OBJECTS)
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS) -lgcc

# Kernel object rules write into $(KOBJ) (bin/k64/).
$(KOBJ)%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) -c $(CXXFLAGS) -MMD -MP $< -o $@
# The shared VT engine (user/term/vt.c) compiled with KERNEL flags for FbConsole. A distinct
# object name (vtk.o) keeps it separate from the userland bin/vt.o that nterm/terminal link.
$(KOBJ)vtk.o: user/term/vt.c
	@mkdir -p $(@D)
	$(CXX) -c $(CXXFLAGS) -MMD -MP $< -o $@
$(KOBJ)%.o: %.s
	@mkdir -p $(@D)
	$(AS) $(ASFLAGS) $< -o $@
$(KOBJ)%.o: %.S
	@mkdir -p $(@D)
	nasm -f $(ASM_FMT) $< -o $@

# ---- x86_64 Plan 2 staged bring-up ------------------------------------------------------
# The staged image links a MINIMAL slice of MI + x86_64 MD (Console + memory_manager +
# bootinfo + a staged Kernel::start) — NOT the full kernel. Its objects are elf64 and MUST NOT
# share bin/ with the i686 elf32 objects of the SAME basename (Console.o, memory_manager.o,
# string_funcs.o, kmain.o, ...), so they build into a SEPARATE dir bin/stage64/. That also
# sidesteps stale-mtime format mismatches when switching ARCH between full builds.
STAGE_BIN=$(BINFOLDER)stage64/
# Plan 4 brings in the real cpu_x86_64.o (full <arch/cpu.h> impl), which supersedes the Plan-2
# bringup_stubs64.o shim (cpuDisableInterrupts/cpuHalt) — so bringup_stubs64.o is dropped here to
# avoid a duplicate-symbol clash at link.
STAGE64_OBJS=loader64.o entry64.o console_x86_64.o bootinfo_x86_64.o \
             MultibootMmap.o kmain.o KernelStage64.o Console.o memory_manager.o Heap.o \
             string_funcs.o icxxabi.o AddressSpace.o mmu_x86_64.o FrameAllocator.o \
             Gdt64.o Idt64.o Interrupt64.o isr64.o irq64.o \
             cpu_x86_64.o fault_x86_64.o irq_x86_64.o irqtest64.o \
             String.o List.o Vfs.o DeviceManager.o ExtFilesystem.o \
             Crc32c.o BlockCache.o ExtCsum.o ExtAllocator.o Journal.o \
             ATA64.o Hdd64.o AtaBlockDevice64.o block_x86_64.o \
             syscall_entry64.o usermode_x86_64.o nxjmp64.o NxeLoader.o
STAGE64_PATHS=$(addprefix $(STAGE_BIN),$(STAGE64_OBJS))

# Staged compile rules write into bin/stage64/ (NOT bin/). The stage pattern's stem is shorter
# than the generic bin/%.o pattern's for a bin/stage64/X.o target, so make prefers it.
$(STAGE_BIN)%.o: %.cpp
	@mkdir -p $(STAGE_BIN)
	$(CXX) -c $(CXXFLAGS) -MMD -MP $< -o $@
# loader64.o from loader.S: the source basename is `loader`, but the 64-suffixed object keeps
# the elf64 boot object from ever colliding with i686's bin/loader.o (an explicit recipe, not
# the generic %.S rule, because basenames differ).
$(STAGE_BIN)loader64.o: arch/x86_64/boot/loader.S
	@mkdir -p $(STAGE_BIN)
	nasm -f $(ASM_FMT) $< -o $@
# Staged NASM rule: isr64.S -> bin/stage64/isr64.o, irq64.S -> bin/stage64/irq64.o (basenames
# are unique, so this generic staged .S pattern doesn't collide with the loader64 recipe).
$(STAGE_BIN)%.o: %.S
	@mkdir -p $(STAGE_BIN)
	nasm -f $(ASM_FMT) $< -o $@

# Link the staged set. -lgcc covers any compiler helper routines ($(LD) = $(CROSS)gcc).
_stage64: $(STAGE64_PATHS)
	$(LD) -T$(ARCH_LINKER) -nostdlib -nostartfiles -o $(BINFOLDER)kernel64.bin $(STAGE64_PATHS) -lgcc

-include $(STAGE64_PATHS:.o=.d)

# Plan 2: build the STAGED long-mode kernel (MI Console + memory_manager + bootinfo + staged
# Kernel::start), then wrap it in the GRUB rescue ISO. Sources are compiled from a
# case-sensitive copy ($(KSRC)) because Console/memory_manager #include <string.h> (the
# bind-mounted macOS FS is case-insensitive, where it would collide with lib/String.h). The
# objects land in /src/bin/stage64 via the $(KSRC)/bin -> /src/bin symlink, isolated from the
# i686 elf32 objects in /src/bin.
_bringup64:
	@mkdir -p $(STAGE_BIN)
	@rm -rf $(KSRC) && mkdir -p $(KSRC) && \
	 tar -cf - --exclude=.git --exclude=disk --exclude=bin --exclude=iso --exclude=coverage --exclude=tests -C /src . | tar -xf - -C $(KSRC) && \
	 ln -s /src/$(BINFOLDER) $(KSRC)/bin
	$(MAKE) -C $(KSRC) _stage64
	@rm -rf /tmp/iso64 && mkdir -p /tmp/iso64/boot/grub
	@cp $(BINFOLDER)kernel64.bin /tmp/iso64/boot/kernel64.bin
	@printf 'set timeout=0\nset default=0\nmenuentry "nanos64" {\n  multiboot /boot/kernel64.bin\n  boot\n}\n' > /tmp/iso64/boot/grub/grub.cfg
	grub-mkrescue -o $(BINFOLDER)nanos64.iso /tmp/iso64

# Plan 9 (un-stage): build the REAL machine-independent kernel via the full `_all` link (into
# bin/kernel.bin — the same kernel/Kernel.cpp the i686 build runs) and install it into a GRUB2
# ext4 DISK image. Depends on _all (which compiles the x86_64 objects into bin/k64/ and links
# bin/kernel.bin). Then: (re)create the disk skeleton (create-image.sh, IMAGE_PATH
# overridden to the x86_64 image), point grub.cfg at the real kernel, and write it to
# /nanos/core/kernel.bin. The ext4 partition doubles as /disks/main, which the real kernel
# mounts at boot; init.nxe (PID 1) is installed at /nanos/core/init.nxe — the exact path
# kernel/Kernel.cpp execs. (The staged kernel + its disk path are retired here; bringup64
# remains as the staged rescue-ISO smoke target.)
_image64: _all _userland64 _kext
	# Hybrid GPT image bootable under BOTH BIOS and UEFI via Limine (limine.conf on the ESP points at
	# /nanos/core/kernel.bin on the ext4 root by label — no /boot/grub/grub.cfg needed).
	IMAGE_PATH=$(IMAGE64) NANOS_BOOT=limine ./scripts/create-image.sh
	# System volume skeleton (mirror of the retired i686 _image): /nanos/{core,bin,lib,kext,config,cache,logs,
	# share/terminfo/x} + the /apps bundle root + the /bin link farm, created upfront so every
	# subsequent install step (and the optional-app blocks below) finds its parent directory.
	-printf "mkdir /nanos\nmkdir /nanos/core\nmkdir /nanos/bin\nmkdir /nanos/lib\nmkdir /nanos/kext\nmkdir /nanos/firmware\nmkdir /nanos/config\nmkdir /nanos/cache\nmkdir /nanos/logs\nmkdir /nanos/share\nmkdir /nanos/share/icons\nmkdir /nanos/share/terminfo\nmkdir /nanos/share/terminfo/x\nmkdir /apps\nmkdir /bin\n" | debugfs -w "$(IMAGE64_PART)" 2>/dev/null
	# /nanos/logs must be world-writable: the i915test tee (/nanos/logs/i915test.txt) is
	# written by the logged-in user, not root; the kernel's i915-boot.txt tee bypasses DAC.
	-printf "set_inode_field /nanos/logs mode 040777\n" | debugfs -w "$(IMAGE64_PART)" 2>/dev/null
	# Optional device-firmware blobs (request_firmware reads /nanos/firmware/<name>). Empty by
	# default: Gen9 i915 needs no GuC/HuC/DMC blob. Any blob shipped here must be redistributable
	# (linux-firmware licence). The README documents the contract.
	printf 'NanOS device firmware.\n\nrequest_firmware(name) reads /nanos/firmware/<name>. Empty by default;\nGen9 (Comet Lake UHD) i915 runs execlists with no GuC/HuC/DMC blob. Any blob\nplaced here must be redistributable (linux-firmware licence).\n' > $(BINFOLDER)firmware-README
	printf "rm /nanos/firmware/README\nwrite $(BINFOLDER)firmware-README /nanos/firmware/README\n" | debugfs -w "$(IMAGE64_PART)" 2>/dev/null || true
	# Optional i915 DMC blob (Comet Lake -> kbl_dmc_ver1_04.bin). Staged ONLY if present under
	# bin/firmware/i915/ (fetch it there out-of-band; it is redistributable linux-firmware). Absent =
	# skipped, so the empty-by-default contract holds; present = DC5/DC6 display power states work
	# without a separate reflash. The same blob rides the fast loop via scripts/update-dell.sh.
	if [ -f $(BINFOLDER)firmware/i915/kbl_dmc_ver1_04.bin ]; then \
	  printf "mkdir /nanos/firmware/i915\nrm /nanos/firmware/i915/kbl_dmc_ver1_04.bin\nwrite $(BINFOLDER)firmware/i915/kbl_dmc_ver1_04.bin /nanos/firmware/i915/kbl_dmc_ver1_04.bin\n" | debugfs -w "$(IMAGE64_PART)" 2>/dev/null || true; \
	fi
	printf "rm /nanos/core/kernel.bin\nwrite $(KOBJ)kernel.bin /nanos/core/kernel.bin\n" | debugfs -w "$(IMAGE64_PART)"
	# Loadable kernel modules (.nkext) -> /nanos/kext; the kernel scans + loads them at boot
	# (loadAllKexts). The PS/2 keyboard + mouse + e1000 NIC drivers live here, NOT in kernel.bin.
	for m in $(KEXTS); do \
	  printf "rm /nanos/kext/$$m.nkext\nwrite $(BINFOLDER)$$m.nkext /nanos/kext/$$m.nkext\n" | debugfs -w "$(IMAGE64_PART)"; \
	done
	# PID 1: the real user/init.c (dynamically linked against libc.ndl), which execve()s the
	# login shell. The kernel execs /disks/main/nanos/core/init.nxe and runs it in ring 3.
	printf "rm /nanos/core/init.nxe\nwrite $(BINFOLDER)init.nxe /nanos/core/init.nxe\n" | debugfs -w "$(IMAGE64_PART)"
	printf "set_inode_field /nanos/core/init.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_PART)"   # executables need +x (exec-perm enforced)
	# The shared C library the dynamic loader binds every program against -> /nanos/lib.
	printf "rm /nanos/lib/libc.ndl\nwrite $(BINFOLDER)libc.ndl /nanos/lib/libc.ndl\n" | debugfs -w "$(IMAGE64_PART)"
	# The shell + the sbase coreutils -> /nanos/bin (nsh resolves a bare command name here first).
	for p in $(X64_SYS_PROGS); do \
	  printf "rm /nanos/bin/$$p.nxe\nwrite $(BINFOLDER)$$p.nxe /nanos/bin/$$p.nxe\nset_inode_field /nanos/bin/$$p.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_PART)"; \
	done
	# chsh edits the account DB, so it runs setuid-root (a non-root user changing their own shell).
	printf "set_inode_field /nanos/bin/chsh.nxe mode 0104755\n" | debugfs -w "$(IMAGE64_PART)"
	# nanosu is the "authenticate to open" privileged helper: it verifies root's password and runs the
	# target as root, so it must be setuid-root (owner root, mode 04755).
	printf "set_inode_field /nanos/bin/nanosu.nxe mode 0104755\n" | debugfs -w "$(IMAGE64_PART)"
	# nanowm compositor (a system GUI program) -> /nanos/bin, and the nanowm client shared libs
	# (libnw.ndl / libnwui.ndl) -> /nanos/lib (the NetSurf libnsfb backend binds libnw.ndl at load).
	for p in $(X64_GUI_PROGS); do \
	  printf "rm /nanos/bin/$$p.nxe\nwrite $(BINFOLDER)$$p.nxe /nanos/bin/$$p.nxe\nset_inode_field /nanos/bin/$$p.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_PART)"; \
	done
	for l in $(X64_GUI_LIBS); do \
	  printf "rm /nanos/lib/$$l\nwrite $(BINFOLDER)$$l /nanos/lib/$$l\n" | debugfs -w "$(IMAGE64_PART)"; \
	done
	# nanowm desktop apps -> /apps/<name>/<name>.nxe bundles + /bin/<name>.nxe symlink (the link farm),
	# the layout nwm spawns them from. Mirrors the retired i686 APP_PROGS loop.
	for p in $(X64_GUI_APPS); do \
	  printf "mkdir /apps/$$p\n" | debugfs -w "$(IMAGE64_PART)" 2>/dev/null; \
	  printf "rm /apps/$$p/$$p.nxe\nwrite $(BINFOLDER)$$p.nxe /apps/$$p/$$p.nxe\nset_inode_field /apps/$$p/$$p.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_PART)"; \
	  printf "rm /bin/$$p.nxe\n" | debugfs -w "$(IMAGE64_PART)" 2>/dev/null; \
	  printf "symlink /bin/$$p.nxe /apps/$$p/$$p.nxe\n" | debugfs -w "$(IMAGE64_PART)"; \
	done
	# Desktop artwork. The wallpaper ships as the source PNG; nanowm decodes it and cover-fits it to
	# the live resolution at runtime (so it fills ANY panel). The logo stays a fixed 96x96 raw (About
	# blits it directly, no scaling). Mirrors the retired i686 _image artwork block.
	if [ -f assets/wallpaper.png ]; then \
	  printf "rm /nanos/share/wallpaper.png\nwrite assets/wallpaper.png /nanos/share/wallpaper.png\n" | debugfs -w "$(IMAGE64_PART)"; \
	fi
	if [ -f $(BINFOLDER)logo.raw ]; then \
	  printf "rm /nanos/share/logo.raw\nwrite $(BINFOLDER)logo.raw /nanos/share/logo.raw\n" | debugfs -w "$(IMAGE64_PART)"; \
	fi
	# Icon set for the Rust file explorer (rsexp) -> /nanos/share/icons. Decoded at runtime via the
	# toolkit PNG loader; the iconview color-keys magenta as transparent.
	for ic in computer drive folder home program text image file ui-back ui-fwd ui-up ui-home ui-computer ui-drive ui-search ui-shield; do \
	  if [ -f assets/icons/$$ic.png ]; then \
	    printf "rm /nanos/share/icons/$$ic.png\nwrite assets/icons/$$ic.png /nanos/share/icons/$$ic.png\n" | debugfs -w "$(IMAGE64_PART)" 2>/dev/null; \
	  fi; \
	done
	# UI/terminal fonts (TTF) -> /nanos/share/fonts; nw_gfx loads the UI font (proportional, AA).
	-printf "mkdir /nanos/share/fonts\n" | debugfs -w "$(IMAGE64_PART)" 2>/dev/null
	for ft in UISans-Regular Mono-Regular; do \
	  if [ -f assets/fonts/$$ft.ttf ]; then \
	    printf "rm /nanos/share/fonts/$$ft.ttf\nwrite assets/fonts/$$ft.ttf /nanos/share/fonts/$$ft.ttf\n" | debugfs -w "$(IMAGE64_PART)" 2>/dev/null; \
	  fi; \
	done
	# Account database -> /nanos/config (init's getpwuid reads pw_shell from here; absent -> nsh).
	# passwd (x in field 2), shadow (hashes, 0600 root), group (with members), sudoers (%wheel).
	# The kernel copies all four into the writable /etc tmpfs at boot (Kernel.cpp populateEtc).
	printf "rm /nanos/config/passwd\nwrite config/passwd /nanos/config/passwd\n" | debugfs -w "$(IMAGE64_PART)"
	printf "rm /nanos/config/shadow\nwrite config/shadow /nanos/config/shadow\n" | debugfs -w "$(IMAGE64_PART)"
	printf "rm /nanos/config/group\nwrite config/group /nanos/config/group\n" | debugfs -w "$(IMAGE64_PART)"
	printf "rm /nanos/config/sudoers\nwrite config/sudoers /nanos/config/sudoers\n" | debugfs -w "$(IMAGE64_PART)"
	-printf "set_inode_field /nanos/config/shadow mode 0100600\nset_inode_field /nanos/config/sudoers mode 0100440\n" | debugfs -w "$(IMAGE64_PART)" 2>/dev/null
	# jan's home directory under /users (NanOS/macOS layout, not Linux /home), uid/gid 1000 so the
	# login shell can write there. Remove any stale /home from an earlier (incremental) build.
	-printf "rmdir /home/jan\nrmdir /home\n" | debugfs -w "$(IMAGE64_PART)" 2>/dev/null
	-printf "mkdir /users\nmkdir /users/jan\n" | debugfs -w "$(IMAGE64_PART)" 2>/dev/null
	-printf "set_inode_field /users/jan uid 1000\nset_inode_field /users/jan gid 1000\nset_inode_field /users/jan mode 040755\n" | debugfs -w "$(IMAGE64_PART)" 2>/dev/null
	# Network/login config templates -> /nanos/config/etc (kernel copies them into the writable /etc
	# tmpfs at boot, see Kernel.cpp populateEtc). /etc/shells in particular lists the valid login
	# shells: dropbear's getusershell() rejects an SSH login whose passwd shell isn't there. Mirrors
	# the retired i686 _image etc population.
	-printf "mkdir /nanos/config/etc\n" | debugfs -w "$(IMAGE64_PART)" 2>/dev/null
	for f in resolv.conf hosts nsswitch.conf protocols services inetd.conf shells profile; do \
	  printf "rm /nanos/config/etc/$$f\nwrite config/etc/$$f /nanos/config/etc/$$f\n" | debugfs -w "$(IMAGE64_PART)"; \
	done
	# DHCP: the udhcpc action helper (compiled .nxe; udhcpc exec()s it) -> /nanos/config/udhcpc.script,
	# plus the busybox udhcpc client itself -> /nanos/bin (only if `make udhcpc` staged it). Mirrors
	# the retired i686 _image DHCP population.
	printf "rm /nanos/config/udhcpc.script\nwrite $(BINFOLDER)dhcpcfg.nxe /nanos/config/udhcpc.script\nset_inode_field /nanos/config/udhcpc.script mode 0100755\n" | debugfs -w "$(IMAGE64_PART)"
	if [ -f $(BINFOLDER)udhcpc.nxe ]; then \
	  printf "rm /nanos/bin/udhcpc.nxe\nwrite $(BINFOLDER)udhcpc.nxe /nanos/bin/udhcpc.nxe\nset_inode_field /nanos/bin/udhcpc.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_PART)"; \
	fi
	# Per-user interactive bash config: seed jan's ~/.bashrc from config/skel (Linux /etc/skel).
	printf "rm /users/jan/.bashrc\nwrite config/skel/.bashrc /users/jan/.bashrc\nset_inode_field /users/jan/.bashrc uid 1000\nset_inode_field /users/jan/.bashrc gid 1000\n" | debugfs -w "$(IMAGE64_PART)"
	# GNU bash (optional): installed as an /apps/bash bundle + a /bin/bash.nxe symlink ONLY if
	# `make ARCH=x86_64 bash` staged bin/bash.nxe. passwd's login shell is /disks/main/bin/bash.nxe,
	# so this is what PID 1 execve()s. Mirrors the retired i686 _image bash population. Skipped silently
	# otherwise (init falls back to nsh). The /apps + /bin link-farm dirs are created here.
	-printf "mkdir /apps\nmkdir /bin\n" | debugfs -w "$(IMAGE64_PART)" 2>/dev/null
	if [ -f $(BINFOLDER)bash.nxe ]; then \
	  printf "mkdir /apps/bash\n" | debugfs -w "$(IMAGE64_PART)" 2>/dev/null; \
	  printf "rm /apps/bash/bash.nxe\nwrite $(BINFOLDER)bash.nxe /apps/bash/bash.nxe\nset_inode_field /apps/bash/bash.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_PART)"; \
	  printf "rm /bin/bash.nxe\n" | debugfs -w "$(IMAGE64_PART)" 2>/dev/null; \
	  printf "symlink /bin/bash.nxe /apps/bash/bash.nxe\n" | debugfs -w "$(IMAGE64_PART)"; \
	fi
	# grep + bzip2 (optional, external): system utilities -> /nanos/bin, installed only if
	# `make ARCH=x86_64 grep|bzip2` staged them. Mirrors the retired i686 _image population.
	if [ -f $(BINFOLDER)grep.nxe ]; then \
	  printf "rm /nanos/bin/grep.nxe\nwrite $(BINFOLDER)grep.nxe /nanos/bin/grep.nxe\nset_inode_field /nanos/bin/grep.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_PART)"; \
	fi
	if [ -f $(BINFOLDER)bzip2.nxe ]; then \
	  printf "rm /nanos/bin/bzip2.nxe\nwrite $(BINFOLDER)bzip2.nxe /nanos/bin/bzip2.nxe\nset_inode_field /nanos/bin/bzip2.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_PART)"; \
	fi
	# htop (optional, external): interactive process monitor built by `make ARCH=x86_64 htop` (the
	# nanos-sdk port), staged into bin/htop.nxe. A system utility (flat in /nanos/bin). Skipped if absent.
	if [ -f $(BINFOLDER)htop.nxe ]; then \
	  printf "rm /nanos/bin/htop.nxe\nwrite $(BINFOLDER)htop.nxe /nanos/bin/htop.nxe\nset_inode_field /nanos/bin/htop.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_PART)"; \
	fi
	# gles2info (optional, external): the Mesa (gallium-virgl + EGL + GLES2) bring-up oracle, built by
	# `make gles2info` (links the Mesa .a closure from the mesa-port). A system utility (flat in
	# /nanos/bin). Skipped if absent — only present on a GL dev build.
	if [ -f $(BINFOLDER)gles2info.nxe ]; then \
	  printf "rm /nanos/bin/gles2info.nxe\nwrite $(BINFOLDER)gles2info.nxe /nanos/bin/gles2info.nxe\nset_inode_field /nanos/bin/gles2info.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_PART)"; \
	fi
	# glkms (optional, external): the Task-9 GBM+EGL+KMS present oracle (GL gradient → scanout), built
	# by `make glkms` (Mesa .a closure + libdrm KMS). A system utility (flat in /nanos/bin). Skipped if
	# absent — only present on a GL dev build.
	if [ -f $(BINFOLDER)glkms.nxe ]; then \
	  printf "rm /nanos/bin/glkms.nxe\nwrite $(BINFOLDER)glkms.nxe /nanos/bin/glkms.nxe\nset_inode_field /nanos/bin/glkms.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_PART)"; \
	fi
	# SQLite (optional, external): the real sqlite3 CLI built by `make ARCH=x86_64 sqlite` (the SQLite
	# fork). A system utility (flat in /nanos/bin); the libsqlite.ndl shared engine goes to /nanos/lib
	# so other apps can import the SQL API by name. Skipped if absent.
	if [ -f $(BINFOLDER)sqlite3.nxe ]; then \
	  printf "rm /nanos/bin/sqlite3.nxe\nwrite $(BINFOLDER)sqlite3.nxe /nanos/bin/sqlite3.nxe\nset_inode_field /nanos/bin/sqlite3.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_PART)"; \
	fi
	if [ -f $(BINFOLDER)libsqlite.ndl ]; then \
	  printf "rm /nanos/lib/libsqlite.ndl\nwrite $(BINFOLDER)libsqlite.ndl /nanos/lib/libsqlite.ndl\n" | debugfs -w "$(IMAGE64_PART)"; \
	fi
	# toybox (optional, external): the user-identity multicall built by `make ARCH=x86_64 toybox`.
	# Installed as ONE setuid-root binary (mode 04755) with a per-command symlink farm — toybox's
	# CONFIG_TOYBOX_SUID drops privilege for the non-suid applets (id/groups/whoami) while
	# login/su/passwd keep root to read /etc/shadow + switch identity. Skipped if absent.
	if [ -f $(BINFOLDER)toybox.nxe ]; then \
	  printf "rm /nanos/bin/toybox.nxe\nwrite $(BINFOLDER)toybox.nxe /nanos/bin/toybox.nxe\nset_inode_field /nanos/bin/toybox.nxe mode 0104755\n" | debugfs -w "$(IMAGE64_PART)"; \
	  for c in login su passwd id groups whoami; do \
	    printf "rm /nanos/bin/$$c.nxe\nln /nanos/bin/toybox.nxe /nanos/bin/$$c.nxe\n" | debugfs -w "$(IMAGE64_PART)"; \
	  done; \
	fi
	# sudo (optional, external): real sudo built by `make ARCH=x86_64 sudo`. Installed setuid-root
	# (mode 04755) so an unprivileged user can escalate; it reads /etc/sudoers (%wheel, seeded).
	if [ -f $(BINFOLDER)sudo.nxe ]; then \
	  printf "rm /nanos/bin/sudo.nxe\nwrite $(BINFOLDER)sudo.nxe /nanos/bin/sudo.nxe\nset_inode_field /nanos/bin/sudo.nxe mode 0104755\n" | debugfs -w "$(IMAGE64_PART)"; \
	fi
	# ping (optional, external): GNU inetutils ping built by `make ARCH=x86_64 ping` (the nanos-sdk
	# port) and staged into bin/ping.nxe. A system utility (flat in /nanos/bin). Skipped if absent.
	# Mirrors the retired i686 _image population.
	if [ -f $(BINFOLDER)ping.nxe ]; then \
	  printf "rm /nanos/bin/ping.nxe\nwrite $(BINFOLDER)ping.nxe /nanos/bin/ping.nxe\nset_inode_field /nanos/bin/ping.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_PART)"; \
	fi
	# openssl (optional, external): OpenSSL CLI built by `make ARCH=x86_64 openssl` (the nanos-sdk
	# port), staged into bin/openssl.nxe. A system utility (flat in /nanos/bin). Skipped if absent.
	# Mirrors the retired i686 _image population.
	if [ -f $(BINFOLDER)openssl.nxe ]; then \
	  printf "rm /nanos/bin/openssl.nxe\nwrite $(BINFOLDER)openssl.nxe /nanos/bin/openssl.nxe\nset_inode_field /nanos/bin/openssl.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_PART)"; \
	fi
	# CA trust store + config (Mozilla bundle): OpenSSL's compiled OPENSSLDIR is /disks/main/nanos/ssl;
	# ship cert.pem + openssl.cnf there so the TLS clients can verify chains without a per-command
	# -CAfile and the CLI finds its config. Mirrors the retired i686 _image population. Skipped if absent.
	if [ -f disk-content/ssl/cert.pem ]; then \
	  printf "mkdir /nanos/ssl\n" | debugfs -w "$(IMAGE64_PART)" 2>/dev/null; \
	  printf "rm /nanos/ssl/cert.pem\nwrite disk-content/ssl/cert.pem /nanos/ssl/cert.pem\n" | debugfs -w "$(IMAGE64_PART)"; \
	  printf "rm /nanos/ssl/openssl.cnf\nwrite disk-content/ssl/openssl.cnf /nanos/ssl/openssl.cnf\n" | debugfs -w "$(IMAGE64_PART)"; \
	fi
	# Dropbear SSH (optional, external): server + keygen + client -> /nanos/bin, installed only if
	# `make ARCH=x86_64 dropbear` staged them. init's start_sshd() generates a persistent ed25519
	# host key under /nanos/config on first boot and launches `dropbear -r .. -p 22`; login is the
	# passwd shell (bash) over the kernel PTY. Also create root's home + .ssh (pubkey authorized_keys
	# location). Mirrors the retired i686 _image dropbear population. The host reaches sshd via hostfwd 2222->22.
	if [ -f $(BINFOLDER)dropbear.nxe ]; then \
	  for b in dropbear dropbearkey dbclient; do \
	    test -f $(BINFOLDER)$$b.nxe && printf "rm /nanos/bin/$$b.nxe\nwrite $(BINFOLDER)$$b.nxe /nanos/bin/$$b.nxe\nset_inode_field /nanos/bin/$$b.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_PART)"; \
	  done; \
	  printf "mkdir /root\nmkdir /root/.ssh\n" | debugfs -w "$(IMAGE64_PART)" 2>/dev/null; \
	fi
	# GNU git (optional, external): installed ONLY if `make git` staged bin/git.nxe. The single
	# binary goes to TWO places: /nanos/bin/git.nxe (the shell runs `git` -> .nxe by name) AND
	# /nanos/libexec/git-core/git.nxe — git's compiled exec-path, where run-command self-execs
	# git-<cmd> for forked subcommands (git gc -> git pack-objects). Mirrors the retired i686 _image.
	if [ -f $(BINFOLDER)git.nxe ]; then \
	  printf "rm /nanos/bin/git.nxe\nwrite $(BINFOLDER)git.nxe /nanos/bin/git.nxe\nset_inode_field /nanos/bin/git.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_PART)"; \
	  printf "mkdir /nanos/libexec\nmkdir /nanos/libexec/git-core\n" | debugfs -w "$(IMAGE64_PART)" 2>/dev/null; \
	  printf "rm /nanos/libexec/git-core/git.nxe\nwrite $(BINFOLDER)git.nxe /nanos/libexec/git-core/git.nxe\nset_inode_field /nanos/libexec/git-core/git.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_PART)"; \
	  for c in gc repack pack-objects pack-refs prune prune-packed reflog rerere worktree maintenance commit-graph multi-pack-index fsck update-server-info upload-pack receive-pack; do \
	    printf "rm /nanos/libexec/git-core/git-%s.nxe\nln /nanos/libexec/git-core/git.nxe /nanos/libexec/git-core/git-%s.nxe\n" "$$c" "$$c" | debugfs -w "$(IMAGE64_PART)" 2>/dev/null; \
	  done; \
	  echo "  installed git -> /nanos/bin/git.nxe + /nanos/libexec/git-core/{git,git-<cmd>}.nxe"; fi
	# Node.js (optional, external): node.nxe built by `make node`, staged into bin/node.nxe. Installed
	# to /nanos/bin (the shell runs `node` -> .nxe by name) plus the acceptance oracle at
	# /apps/node-smoke/node-smoke.js (plan 02). Skipped if absent.
	if [ -f $(BINFOLDER)node.nxe ]; then \
	  printf "rm /nanos/bin/node.nxe\nwrite $(BINFOLDER)node.nxe /nanos/bin/node.nxe\nset_inode_field /nanos/bin/node.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_PART)"; \
	  printf "mkdir /apps/node-smoke\n" | debugfs -w "$(IMAGE64_PART)" 2>/dev/null; \
	  printf "rm /apps/node-smoke/node-smoke.js\nwrite scripts/electron/node-smoke.js /apps/node-smoke/node-smoke.js\n" | debugfs -w "$(IMAGE64_PART)"; \
	  echo "  installed node -> /nanos/bin/node.nxe + /apps/node-smoke/node-smoke.js"; fi
	# wget (optional, external): GNU wget built by `make wget`, staged into bin/wget.nxe. A system
	# utility (flat in /nanos/bin). Skipped if absent. Mirrors the retired i686 _image population.
	if [ -f $(BINFOLDER)wget.nxe ]; then \
	  printf "rm /nanos/bin/wget.nxe\nwrite $(BINFOLDER)wget.nxe /nanos/bin/wget.nxe\nset_inode_field /nanos/bin/wget.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_PART)"; \
	fi
	# inetd + telnetd (optional, external): GNU inetutils services built by `make inetd`. telnetd is
	# launched by inetd from /etc/inetd.conf (execs nanologin -> the user's shell over a kernel pty).
	if [ -f $(BINFOLDER)inetd.nxe ]; then \
	  printf "rm /nanos/bin/inetd.nxe\nwrite $(BINFOLDER)inetd.nxe /nanos/bin/inetd.nxe\nset_inode_field /nanos/bin/inetd.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_PART)"; \
	fi
	if [ -f $(BINFOLDER)telnetd.nxe ]; then \
	  printf "rm /nanos/bin/telnetd.nxe\nwrite $(BINFOLDER)telnetd.nxe /nanos/bin/telnetd.nxe\nset_inode_field /nanos/bin/telnetd.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_PART)"; \
	fi
	# inetutils diagnostic clients: telnet / ifconfig / traceroute -> /nanos/bin. Skipped if absent.
	for b in telnet ifconfig traceroute; do \
	  if [ -f $(BINFOLDER)$$b.nxe ]; then \
	    printf "rm /nanos/bin/$$b.nxe\nwrite $(BINFOLDER)$$b.nxe /nanos/bin/$$b.nxe\nset_inode_field /nanos/bin/$$b.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_PART)"; \
	  fi; \
	done
	# darkhttpd (optional, external): single-file HTTP server -> /nanos/bin, plus its document
	# root /apps/www (the served site: index.html). Skipped if the binary is absent.
	if [ -f $(BINFOLDER)darkhttpd.nxe ]; then \
	  printf "rm /nanos/bin/darkhttpd.nxe\nwrite $(BINFOLDER)darkhttpd.nxe /nanos/bin/darkhttpd.nxe\nset_inode_field /nanos/bin/darkhttpd.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_PART)"; \
	  printf "mkdir /apps/www\n" | debugfs -w "$(IMAGE64_PART)" 2>/dev/null; \
	  printf "rm /apps/www/index.html\nwrite disk-content/www/index.html /apps/www/index.html\n" | debugfs -w "$(IMAGE64_PART)"; \
	fi
	# vim (optional, external): an /apps/vim bundle + a /bin/vim.nxe symlink + its runtime
	# defaults.vim, only if `make ARCH=x86_64 vim` staged it. Mirrors the retired i686 _image bundle.
	if [ -f $(BINFOLDER)vim.nxe ]; then \
	  printf "mkdir /apps/vim\n" | debugfs -w "$(IMAGE64_PART)" 2>/dev/null; \
	  printf "rm /apps/vim/vim.nxe\nwrite $(BINFOLDER)vim.nxe /apps/vim/vim.nxe\nset_inode_field /apps/vim/vim.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_PART)"; \
	  printf "rm /bin/vim.nxe\n" | debugfs -w "$(IMAGE64_PART)" 2>/dev/null; \
	  printf "symlink /bin/vim.nxe /apps/vim/vim.nxe\n" | debugfs -w "$(IMAGE64_PART)"; \
	  printf "mkdir /apps/vim/runtime\n" | debugfs -w "$(IMAGE64_PART)" 2>/dev/null; \
	  printf "rm /apps/vim/runtime/defaults.vim\nwrite user/vim-runtime/defaults.vim /apps/vim/runtime/defaults.vim\n" | debugfs -w "$(IMAGE64_PART)"; \
	  printf "mkdir /nanos/share\nmkdir /nanos/share/terminfo\nmkdir /nanos/share/terminfo/x\n" | debugfs -w "$(IMAGE64_PART)" 2>/dev/null; \
	  infocmp xterm-256color 2>/dev/null \
	    | sed -E 's@setaf=[^,]*,@setaf=\\E[3%p1%dm,@; s@setab=[^,]*,@setab=\\E[4%p1%dm,@' \
	    > /tmp/xterm-256color.ti; \
	  tic -x -o /tmp/nanos-terminfo /tmp/xterm-256color.ti 2>/dev/null; \
	  printf "rm /nanos/share/terminfo/x/xterm-256color\nwrite /tmp/nanos-terminfo/x/xterm-256color /nanos/share/terminfo/x/xterm-256color\n" | debugfs -w "$(IMAGE64_PART)"; \
	fi
	# terminfo DB for vim (mirror of the retired i686 _image): the xterm-256color entry, with setaf/setab rewritten
	# to the SIMPLE \E[3%p1%dm / \E[4%p1%dm form vim's term_color() drives correctly (the stock
	# conditional form leaks junk through vim's minimal tgoto). Shipped under /nanos/share/terminfo,
	# matching TERM=xterm-256color + TERMINFO. Done above, inside the vim block (its only consumer).
	# Doom (optional, in-tree doomgeneric): an /apps/doom bundle (the binary + its shareware
	# IWAD doom1.wad) + a /bin/doom.nxe symlink, installed only if `make ARCH=x86_64 doom`
	# staged bin/doom.nxe. Mirrors the retired i686 _image doom bundle (APP_PROGS + the WAD write), but
	# guarded by file presence since doom is not in the minimal X64_USER_PROGS subset. doom mmaps
	# /dev/fb0 and reads /dev/input0, both now present on x86_64 (the framebuffer multiboot tag).
	if [ -f $(BINFOLDER)doom.nxe ]; then \
	  printf "mkdir /apps/doom\n" | debugfs -w "$(IMAGE64_PART)" 2>/dev/null; \
	  printf "rm /apps/doom/doom.nxe\nwrite $(BINFOLDER)doom.nxe /apps/doom/doom.nxe\nset_inode_field /apps/doom/doom.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_PART)"; \
	  printf "rm /apps/doom/doom1.wad\nwrite disk/doom1.wad /apps/doom/doom1.wad\n" | debugfs -w "$(IMAGE64_PART)"; \
	  printf "rm /bin/doom.nxe\n" | debugfs -w "$(IMAGE64_PART)" 2>/dev/null; \
	  printf "symlink /bin/doom.nxe /apps/doom/doom.nxe\n" | debugfs -w "$(IMAGE64_PART)"; \
	fi
	# NetSurf graphical browser (optional, external): an /apps/netsurf bundle — the .nxe plus its
	# res/ tree (default/quirks/internal CSS, the Messages catalogue, the internal bitmap font,
	# icons, locale dirs) — plus a /bin/netsurf.nxe symlink (the app-bundle + link-farm pattern).
	# res/ is installed recursively (dirs first top-down, then files). Built by `make netsurf`
	# (the netsurf-nanos port stack); launch inside nwm. Skipped if bin/netsurf.nxe is absent.
	if [ -f $(BINFOLDER)netsurf.nxe ]; then \
	  printf "mkdir /apps/netsurf\n" | debugfs -w "$(IMAGE64_PART)" 2>/dev/null; \
	  printf "rm /apps/netsurf/netsurf.nxe\nwrite $(BINFOLDER)netsurf.nxe /apps/netsurf/netsurf.nxe\nset_inode_field /apps/netsurf/netsurf.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_PART)"; \
	  printf "rm /bin/netsurf.nxe\n" | debugfs -w "$(IMAGE64_PART)" 2>/dev/null; \
	  printf "symlink /bin/netsurf.nxe /apps/netsurf/netsurf.nxe\n" | debugfs -w "$(IMAGE64_PART)"; \
	  if [ -d $(BINFOLDER)netsurf-res ]; then \
	    printf "mkdir /apps/netsurf/res\n" | debugfs -w "$(IMAGE64_PART)" 2>/dev/null; \
	    ( cd $(BINFOLDER)netsurf-res && find . -mindepth 1 -type d | sed 's#^\./##' ) | while read d; do \
	      printf "mkdir /apps/netsurf/res/$$d\n" | debugfs -w "$(IMAGE64_PART)" 2>/dev/null; \
	    done; \
	    ( cd $(BINFOLDER)netsurf-res && find . -type f | sed 's#^\./##' ) | while read f; do \
	      printf "rm /apps/netsurf/res/$$f\nwrite $(BINFOLDER)netsurf-res/$$f /apps/netsurf/res/$$f\n" | debugfs -w "$(IMAGE64_PART)"; \
	    done; \
	  fi; \
	fi
	# Reconcile the ext bitmaps after the debugfs writes so the built image is e2fsck-clean
	# (exit 1 = "fixed" is expected here, so don't fail the build on it).
	e2fsck -fy "$(IMAGE64_PART)" || true
	@echo "x86_64 disk image ready: $(IMAGE64)  (boot: $(QEMU) $(QEMU_CPU) $(QEMU_MEM) -drive file=$(IMAGE64),format=raw)"

-include $(OBJECTS:.o=.d)

_clean:
	-rm $(BINFOLDER)*.o $(BINFOLDER)*.d $(BINFOLDER)kernel.bin
	-rm $(BINFOLDER)*.elf $(BINFOLDER)*.nxe $(BINFOLDER)*.ndl $(BINFOLDER)*.a $(BINFOLDER)mknx64
	-rm -rf $(BINFOLDER)libimp $(BINFOLDER)k64

# ----------------------------------------------------------------------------
# Userland: .nxe programs link against ported picolibc + our syscall glue (own
# linker script/base 0x400000, NOT in kernel SOURCES). picolibc headers come via
# -isystem; SyscallNr.h via -Ikernel. No <string.h> clash (lib/ is not on the path).
# ----------------------------------------------------------------------------
# The 64-bit picolibc sysroot built into nanos-build (commit 170f150) + the SysV-AMD64
# userland flags (SSE on for varargs/float, small code model + non-PIC for the fixed low
# base, no red zone).
PICOLIBC=/opt/picolibc/x86_64-elf
USER_ARCHFLAGS=-mcmodel=small -mno-red-zone
SBASE=user/third_party/sbase
# kernel/ is on -iquote (not -I): SyscallNr.h is a "quoted" include, and this keeps the
# new kernel/Signal.h from shadowing picolibc's <signal.h> on the case-insensitive macOS
# bind mount (kernel/Signal.h == <signal.h> under -I, which broke the userland build).
USER_CFLAGS=-ffreestanding -isystem $(PICOLIBC)/include -iquote kernel -Iuser -Iuser/libnw -Iuser/nwm -Iuser/libnwui -Iuser/term -Iuser/libc-glue/include -Iuser/third_party/stb -I$(SBASE) -D_DEFAULT_SOURCE -include user/libc-glue/compat-decls.h -Wall -fno-pic -fno-stack-protector $(USER_ARCHFLAGS) $(UOPTFLAGS)
USER_LIBS=-L$(PICOLIBC)/lib -lc -lgcc
# Shared per-program objects: startup, .nxe header, the picolibc syscall glue, and
# the userland cwd layer (syscalls.o's path resolver lives in cwd.o).
USER_GLUE=$(BINFOLDER)crt0.o $(BINFOLDER)sigtramp.o $(BINFOLDER)nxhdr.o $(BINFOLDER)syscalls.o $(BINFOLDER)cwd.o
# Per-program libutil subsets and the libutf objects (ls needs UTF + dir/pwd glue).
SBASE_UTIL_CAT=$(BINFOLDER)eprintf.o $(BINFOLDER)concat.o $(BINFOLDER)writeall.o
SBASE_UTIL_LS=$(BINFOLDER)eprintf.o $(BINFOLDER)ealloc.o $(BINFOLDER)reallocarray.o $(BINFOLDER)human.o $(BINFOLDER)fshut.o
# Basic coreutils (in-tree sbase ports, see docs/superpowers/plans/2026-06-16-coreutils.md).
SBASE_UTIL_MKDIR=$(BINFOLDER)eprintf.o $(BINFOLDER)mode.o $(BINFOLDER)mkdirp.o $(BINFOLDER)strlcpy.o
SBASE_UTIL_RMDIR=$(BINFOLDER)eprintf.o
SBASE_UTIL_PWD=$(BINFOLDER)eprintf.o $(BINFOLDER)fshut.o
SBASE_UTIL_TOUCH=$(BINFOLDER)eprintf.o $(BINFOLDER)strtonum.o
SBASE_UTIL_RM=$(BINFOLDER)eprintf.o $(BINFOLDER)recurse.o $(BINFOLDER)rm_cb.o $(BINFOLDER)confirm.o $(BINFOLDER)strlcpy.o $(BINFOLDER)ealloc.o
SBASE_UTIL_LN=$(BINFOLDER)eprintf.o
SBASE_UTIL_CP=$(BINFOLDER)eprintf.o $(BINFOLDER)fshut.o $(BINFOLDER)enmasse.o $(BINFOLDER)fnck.o $(BINFOLDER)cp_cb.o $(BINFOLDER)confirm.o $(BINFOLDER)concat.o $(BINFOLDER)writeall.o $(BINFOLDER)strlcpy.o $(BINFOLDER)strlcat.o $(BINFOLDER)ealloc.o
# mv = cp's deps + rm callback (cross-fs fallback does cp then recurse(rm)); same-fs uses rename(2).
SBASE_UTIL_MV=$(BINFOLDER)eprintf.o $(BINFOLDER)enmasse.o $(BINFOLDER)fnck.o $(BINFOLDER)cp_cb.o $(BINFOLDER)rm_cb.o $(BINFOLDER)recurse.o $(BINFOLDER)confirm.o $(BINFOLDER)concat.o $(BINFOLDER)writeall.o $(BINFOLDER)strlcpy.o $(BINFOLDER)strlcat.o $(BINFOLDER)ealloc.o
SBASE_UTIL_CHMOD=$(BINFOLDER)eprintf.o $(BINFOLDER)mode.o $(BINFOLDER)recurse.o $(BINFOLDER)strlcpy.o $(BINFOLDER)ealloc.o
SBASE_UTIL_WC=$(BINFOLDER)eprintf.o $(BINFOLDER)fshut.o
SBASE_UTIL_HEAD=$(BINFOLDER)eprintf.o $(BINFOLDER)fshut.o $(BINFOLDER)strtonum.o
SBASE_UTIL_TAIL=$(BINFOLDER)eprintf.o $(BINFOLDER)fshut.o $(BINFOLDER)strtonum.o $(BINFOLDER)writeall.o $(BINFOLDER)concat.o $(BINFOLDER)ealloc.o
SBASE_UTIL_ENV=$(BINFOLDER)eprintf.o $(BINFOLDER)fshut.o
SBASE_UTIL_BASENAME=$(BINFOLDER)eprintf.o $(BINFOLDER)fshut.o
SBASE_UTIL_DIRNAME=$(BINFOLDER)eprintf.o $(BINFOLDER)fshut.o
LIBUTF_OBJS=$(patsubst $(SBASE)/libutf/%.c,$(BINFOLDER)%.o,$(wildcard $(SBASE)/libutf/*.c))
GLUE_LS=$(BINFOLDER)dirent.o $(BINFOLDER)pwd_grp.o
# Per-program glue for DYNAMICALLY-linked programs: startup + header placeholder only —
# the C library (picolibc + syscall/cwd/signal glue + the signal trampoline) now lives in
# libc.ndl, pulled in by name via the import library instead of static-linked.
DYN_GLUE=$(BINFOLDER)crt0.o $(BINFOLDER)nxhdr.o

# All shipped programs are dynamically linked against libc.ndl, so PROGRAM objects are
# compiled with nx-dllimport.h force-included (it redirects stdio data symbols stdout/
# stderr/stdin/errno through the __imp_ slots). It is added ONLY to the program compile
# rules below — NEVER to the libc-glue rule, since that code lives inside libc.ndl and must
# use the real streams.
DYNHDR=-include user/libc-glue/nx-dllimport.h

# Userland objects build via per-source-dir pattern rules — only CHANGED files recompile
# (the old recipe recompiled all ~30 programs+glue every build), and -MMD tracks header
# deps. Linking is a generic %.nxe rule over each program's declared object prerequisites.
$(BINFOLDER)%.o: user/%.c
	@mkdir -p $(BINFOLDER)
	$(CXX) $(USER_CFLAGS) $(DYNHDR) -MMD -MP -c $< -o $@
$(BINFOLDER)%.o: user/libc-glue/%.c
	$(CXX) $(USER_CFLAGS) -MMD -MP -c $< -o $@
# drmtest links the raw DRM ABI against the VENDORED uapi headers (byte-identical struct/ioctl
# numbers to the driver) + the pinned virgl protocol header — never the host's.
DRMTEST_INC=-Iexternal/linux-6.12/include/uapi -Iexternal/linux-6.12/include -Iexternal/virgl -Iuser/drmtest -Ilinuxkpi/include
$(BINFOLDER)drmtest.o: user/drmtest/drmtest.c
	@mkdir -p $(BINFOLDER)
	$(CXX) $(USER_CFLAGS) $(DRMTEST_INC) $(DYNHDR) -MMD -MP -c $< -o $@
# i915test — the GEM/execbuf oracle (gem_exec_store): softpinned MI_STORE_DWORD_IMM batch on the
# render engine, CPU-verified readback. Same vendored-uapi discipline as drmtest.
$(BINFOLDER)i915test.o: user/i915test/i915test.c
	@mkdir -p $(BINFOLDER)
	$(CXX) $(USER_CFLAGS) $(DRMTEST_INC) $(DYNHDR) -MMD -MP -c $< -o $@
# glpix — the render->scanout oracle. Same vendored-uapi discipline as drmtest, and it reuses
# drmtest's virgl_words.h (build_clear_stream) via -Iuser/drmtest.
$(BINFOLDER)glpix.o: user/glpix/glpix.c
	@mkdir -p $(BINFOLDER)
	$(CXX) $(USER_CFLAGS) $(DRMTEST_INC) $(DYNHDR) -MMD -MP -c $< -o $@
$(BINFOLDER)%.o: $(SBASE)/%.c
	$(CXX) $(USER_CFLAGS) $(DYNHDR) -MMD -MP -c $< -o $@
$(BINFOLDER)%.o: $(SBASE)/libutil/%.c
	$(CXX) $(USER_CFLAGS) $(DYNHDR) -MMD -MP -c $< -o $@
# sbase ships the fs-operation callbacks (rm/cp) in libutil/{rm,cp}.c, whose basenames collide
# with the top-level command files (rm.c/cp.c -> bin/{rm,cp}.o). Compile the libutil callbacks to
# distinct *_cb.o objects so both can link into the command's .nxe.
$(BINFOLDER)rm_cb.o: $(SBASE)/libutil/rm.c
	$(CXX) $(USER_CFLAGS) $(DYNHDR) -MMD -MP -c $< -o $@
$(BINFOLDER)cp_cb.o: $(SBASE)/libutil/cp.c
	$(CXX) $(USER_CFLAGS) $(DYNHDR) -MMD -MP -c $< -o $@
$(BINFOLDER)%.o: $(SBASE)/libutf/%.c
	$(CXX) $(USER_CFLAGS) $(DYNHDR) -MMD -MP -c $< -o $@
$(BINFOLDER)%.o: user/%.S
	@mkdir -p $(BINFOLDER)
	nasm -f $(ASM_FMT) $< -o $@
$(BINFOLDER)%.o: user/libc-glue/%.S
	@mkdir -p $(BINFOLDER)
	nasm -f $(ASM_FMT) $< -o $@
# Vendored musl pthread internals (user/libc-glue/pthread). Compiled with the musl internal
# headers (pthread_impl.h/atomic.h/syscall.h/...) on the include path AHEAD of nothing else
# that defines them, and the weak_alias/hidden compat macros force-included. The .s files are
# GNU-as (AT&T) — the cross gcc assembles them directly.
# The vendored musl headers must out-rank USER_CFLAGS's `-iquote kernel`: the .c files do
# `#include "futex.h"` / `"syscall.h"`, names that ALSO exist under kernel/. Listing the musl
# include dir as `-iquote` FIRST makes it win the quoted-include search. -Wno-unused-value
# silences musl's intentional `__syscall(...) != -ENOSYS || __syscall(...)` fallback idiom.
PTHREAD_CFLAGS=-iquote user/libc-glue/pthread/include -I user/libc-glue/pthread/include \
  $(USER_CFLAGS) -include user/libc-glue/pthread/nanos-musl-compat.h -Wno-unused-value
$(BINFOLDER)%.o: user/libc-glue/pthread/%.c
	@mkdir -p $(BINFOLDER)
	$(CXX) $(PTHREAD_CFLAGS) -MMD -MP -c $< -o $@
$(BINFOLDER)%.o: user/libc-glue/pthread/%.s
	@mkdir -p $(BINFOLDER)
	$(CXX) -c $< -o $@
$(BINFOLDER)%.o: user/term/%.c
	@mkdir -p $(BINFOLDER)
	$(CXX) $(USER_CFLAGS) $(DYNHDR) -MMD -MP -c $< -o $@
# nanowm compositor (user/nwm) + client demos (user/notepad). DYNHDR (libc.ndl import shim) —
# these are dynamically-linked programs like the rest of userland.
$(BINFOLDER)%.o: user/nwm/%.c
	@mkdir -p $(BINFOLDER)
	$(CXX) $(USER_CFLAGS) $(DYNHDR) -MMD -MP -c $< -o $@
$(BINFOLDER)%.o: user/notepad/%.c
	@mkdir -p $(BINFOLDER)
	$(CXX) $(USER_CFLAGS) $(DYNHDR) -MMD -MP -c $< -o $@
$(BINFOLDER)%.o: user/form/%.c
	@mkdir -p $(BINFOLDER)
	$(CXX) $(USER_CFLAGS) $(DYNHDR) -MMD -MP -c $< -o $@
$(BINFOLDER)%.o: user/nwexp/%.c
	@mkdir -p $(BINFOLDER)
	$(CXX) $(USER_CFLAGS) $(DYNHDR) -MMD -MP -c $< -o $@
$(BINFOLDER)%.o: user/settings/%.c
	@mkdir -p $(BINFOLDER)
	$(CXX) $(USER_CFLAGS) $(DYNHDR) -MMD -MP -c $< -o $@
$(BINFOLDER)%.o: user/about/%.c
	@mkdir -p $(BINFOLDER)
	$(CXX) $(USER_CFLAGS) $(DYNHDR) -MMD -MP -c $< -o $@
$(BINFOLDER)%.o: user/viewer/%.c
	@mkdir -p $(BINFOLDER)
	$(CXX) $(USER_CFLAGS) $(DYNHDR) -MMD -MP -c $< -o $@
$(BINFOLDER)%.o: user/nwbench/%.c
	@mkdir -p $(BINFOLDER)
	$(CXX) $(USER_CFLAGS) $(DYNHDR) -MMD -MP -c $< -o $@
$(BINFOLDER)%.o: user/properties/%.c
	@mkdir -p $(BINFOLDER)
	$(CXX) $(USER_CFLAGS) $(DYNHDR) -MMD -MP -c $< -o $@
$(BINFOLDER)%.o: user/open/%.c
	@mkdir -p $(BINFOLDER)
	$(CXX) $(USER_CFLAGS) $(DYNHDR) -MMD -MP -c $< -o $@
$(BINFOLDER)%.o: user/terminal/%.c
	@mkdir -p $(BINFOLDER)
	$(CXX) $(USER_CFLAGS) $(DYNHDR) -MMD -MP -c $< -o $@
# libnw (user/libnw): the client API + protocol codec + gfx, STATICALLY linked into the
# compositor and each GUI client (so it shares that program's libc.ndl). Compiled WITH
# DYNHDR like any program object. nwproto.o/nw_gfx.o are shared by nwm and the clients.
$(BINFOLDER)%.o: user/libnw/%.c
	@mkdir -p $(BINFOLDER)
	$(CXX) $(USER_CFLAGS) $(DYNHDR) -MMD -MP -c $< -o $@
# libnwui (user/libnwui): the UI toolkit, a shared lib like libnw. Same DYNHDR rule.
$(BINFOLDER)%.o: user/libnwui/%.c
	@mkdir -p $(BINFOLDER)
	$(CXX) $(USER_CFLAGS) $(DYNHDR) -MMD -MP -c $< -o $@

# mknx64: host build tool (native cc) that turns the linked ELF64 into a .nxe/.ndl —
# extracts the load image + R_X86_64_* base relocations + exports/imports (v4 layout,
# 64-bit nxaddr_t). Needs the format header (kernel/NxFormat.h) via -Ikernel.
MKNX64=$(BINFOLDER)mknx64
$(MKNX64): tools/mknx.c kernel/NxFormat.h
	@mkdir -p $(BINFOLDER)
	cc -O2 -Wall -DNX_FORCE64 -Ikernel -o $@ tools/mknx.c

MKNX_TOOL=$(MKNX64)
# Userland linker script: elf64-x86-64 OUTPUT_FORMAT + the low fixed base for R_X86_64_32S.
USER_NX_LD=arch/x86_64/user-nx.ld

# ----------------------------------------------------------------------------
# x86_64 minimal in-tree userland (Plan 6): freestanding, SSE ON (decision #3 — SysV AMD64
# requires SSE for varargs/float; the kernel keeps -mno-sse, see arch/x86_64/arch.mk). Small
# code model, non-PIC, fixed low base (decision #1). Built by the KERNEL toolchain ($(CROSS)).
# ----------------------------------------------------------------------------
UARCHFLAGS64 = -mcmodel=small -fno-pic -mno-red-zone   # SSE intentionally NOT disabled
# Like the kernel build, NOT -nostdinc: the bare x86_64-elf cross-gcc ships no libc/sysroot, so
# the only headers it finds are gcc's own freestanding ones (stdint.h — needed by NxFormat.h).
USER64_CFLAGS = -ffreestanding -nostdlib -Ikernel -Iuser $(UARCHFLAGS64) -Wall

# The minimal freestanding 64-bit init (Plan 6) is retired here: now that the full 64-bit
# libc.ndl works in ring 3 (Plan 10 10a/10b), x86_64 builds the REAL user/init.c — the same
# dynamically-linked PID 1 the i686 build runs, which execve()s the login shell. init.nxe
# therefore falls through to the generic dynamic prereq + %.nxe recipe below (arch-selected
# MKNX_TOOL + USER_NX_LD), exactly like nsh and the coreutils. The crt0/sigtramp/pthread .s
# overrides below stay — they are the 64-bit C-library startup the dynamic build links.
bin/init64.elf: user/crt064.o user/nxhdr64.o user/libnanos64.o user/init64.o arch/x86_64/user-nx.ld
	$(CROSS)gcc -nostdlib -Wl,--emit-relocs -T arch/x86_64/user-nx.ld \
	  -o $@ user/crt064.o user/nxhdr64.o user/libnanos64.o user/init64.o

# Object rules for the 64-bit userland (distinct *64.o names avoid clashing with the i686
# bin/*.o during transition; the 64-suffix convention, like loader64.o in Plan 1).
user/crt064.o: user/crt064.S
	nasm -f elf64 $< -o $@
user/sigtramp64.o: user/sigtramp64.S
	nasm -f elf64 $< -o $@
user/nxhdr64.o: user/nxhdr.c
	$(CROSS)gcc $(USER64_CFLAGS) -c $< -o $@
user/libnanos64.o: user/libnanos.c
	$(CROSS)gcc $(USER64_CFLAGS) -c $< -o $@
user/init64.o: user/init64.c
	$(CROSS)gcc $(USER64_CFLAGS) -c $< -o $@

# The DYN_GLUE / libc.ndl chain wants bin/crt0.o (and bin/nxhdr.o) as ELF64. crt0.o carries
# the main-thread TLS bootstrap (calls __nx_init_tls before main, see user/crt064.S); the
# explicit rule (over the generic user/%.S pattern) keeps the nasm format pinned. nxhdr.o
# builds from user/nxhdr.c via the generic user/%.c rule, so it needs no override here.
$(BINFOLDER)crt0.o: user/crt064.S
	@mkdir -p $(BINFOLDER)
	nasm -f elf64 $< -o $@
	# Weaken the user-nx.ld-provided symbols (TLS image + ctors/init_array bounds): autoconf
	# conftests link crt0.o WITHOUT the NanOS linker script, and strong undefined refs made
	# every autotools port fail "cannot compute EXEEXT" the moment crt0 gained TLS/ctor
	# bootstrap. Weak-undefined resolves to 0 in a bare link (conftest never runs _start);
	# real .nxe links keep the script's definitions (a defined symbol beats weak-undef).
	$(CROSS)objcopy $(foreach s,__nx_tls_image __nx_tls_filesz __nx_tls_memsz __nx_tls_align_sym __nx_init_array_start __nx_init_array_end __nx_ctors_start __nx_ctors_end,--weaken-symbol=$(s)) $@

# The signal trampoline: bin/sigtramp.o (in LIBC_GLUE_OBJS) is the 64-bit __nx_sigtramp
# from user/sigtramp64.S (explicit rule over the generic user/%.S pattern).
$(BINFOLDER)sigtramp.o: user/sigtramp64.S
	@mkdir -p $(BINFOLDER)
	nasm -f elf64 $< -o $@

# The two arch-specific musl pthread .s files have x86_64 variants (clone64.s /
# __set_thread_area64.s — the `syscall` insn + arch_prctl FS install). Explicit rules override
# the generic user/libc-glue/pthread/%.s pattern (which would assemble the i386 register-32 .s
# as 64-bit and fail). Assembled by the GNU-as path ($(CXX) -c), like the i686 .s files.
$(BINFOLDER)clone.o: user/libc-glue/pthread/clone64.s
	@mkdir -p $(BINFOLDER)
	$(CXX) -c $< -o $@
$(BINFOLDER)__set_thread_area.o: user/libc-glue/pthread/__set_thread_area64.s
	@mkdir -p $(BINFOLDER)
	$(CXX) -c $< -o $@

# Generic DYNAMIC link: every program links its objects + the import library (NO static
# libc) and declares "needed: libc.ndl", so the loader maps libc.ndl and binds its imports
# by name. `--emit-relocs` keeps the relocations so mknx64 can build the relocation
# table (the .nxe loads at any base). Each program below just declares its object prereqs.
$(BINFOLDER)%.nxe: $(MKNX_TOOL)
	$(LD) -nostdlib -Wl,--emit-relocs -T $(USER_NX_LD) -o $(@:.nxe=.elf) $(filter %.o,$^) $(filter %.a,$^) -lgcc
	$(MKNX_TOOL) $(@:.nxe=.elf) $@ --need libc.ndl

# Per-program object sets: DYN_GLUE (crt0+nxhdr) + program objects + the libc import
# library (an ARCHIVE — the linker pulls only the members the program references, so it
# imports just the symbols it uses). libc.ndl is a prereq so it is built/shipped. Doom has
# an explicit rule (extra math).
DYN_DEPS=$(DYN_GLUE) $(BINFOLDER)libc.ndl.a $(BINFOLDER)libc.ndl
# init.nxe links dynamically against libc.ndl (prereqs here + the generic %.nxe recipe) on
# BOTH arches now: x86_64's full libc.ndl works in ring 3, so PID 1 is the real user/init.c
# that execve()s the shell (the minimal freestanding init64 is retired).
$(BINFOLDER)init.nxe:      $(DYN_DEPS) $(BINFOLDER)init.o
$(BINFOLDER)nsh.nxe:       $(DYN_DEPS) $(BINFOLDER)nsh.o
$(BINFOLDER)open.nxe:      $(DYN_DEPS) $(BINFOLDER)open.o
$(BINFOLDER)nanosu.nxe:      $(DYN_DEPS) $(BINFOLDER)nanosu.o
$(BINFOLDER)free.nxe:      $(DYN_DEPS) $(BINFOLDER)free.o
$(BINFOLDER)chsh.nxe:      $(DYN_DEPS) $(BINFOLDER)chsh.o
$(BINFOLDER)cat.nxe:       $(DYN_DEPS) $(BINFOLDER)cat.o $(SBASE_UTIL_CAT)
$(BINFOLDER)ls.nxe:        $(DYN_DEPS) $(BINFOLDER)ls.o $(SBASE_UTIL_LS) $(LIBUTF_OBJS) $(GLUE_LS)
$(BINFOLDER)mkdir.nxe:     $(DYN_DEPS) $(BINFOLDER)mkdir.o $(SBASE_UTIL_MKDIR)
$(BINFOLDER)rmdir.nxe:     $(DYN_DEPS) $(BINFOLDER)rmdir.o $(SBASE_UTIL_RMDIR)
$(BINFOLDER)pwd.nxe:       $(DYN_DEPS) $(BINFOLDER)pwd.o $(SBASE_UTIL_PWD)
$(BINFOLDER)touch.nxe:     $(DYN_DEPS) $(BINFOLDER)touch.o $(SBASE_UTIL_TOUCH)
$(BINFOLDER)rm.nxe:        $(DYN_DEPS) $(BINFOLDER)rm.o $(SBASE_UTIL_RM)
$(BINFOLDER)ln.nxe:        $(DYN_DEPS) $(BINFOLDER)ln.o $(SBASE_UTIL_LN)
$(BINFOLDER)cp.nxe:        $(DYN_DEPS) $(BINFOLDER)cp.o $(SBASE_UTIL_CP)
$(BINFOLDER)mv.nxe:        $(DYN_DEPS) $(BINFOLDER)mv.o $(SBASE_UTIL_MV)
$(BINFOLDER)chmod.nxe:     $(DYN_DEPS) $(BINFOLDER)chmod.o $(SBASE_UTIL_CHMOD)
$(BINFOLDER)wc.nxe:        $(DYN_DEPS) $(BINFOLDER)wc.o $(SBASE_UTIL_WC) $(LIBUTF_OBJS)
$(BINFOLDER)head.nxe:      $(DYN_DEPS) $(BINFOLDER)head.o $(SBASE_UTIL_HEAD)
$(BINFOLDER)tail.nxe:      $(DYN_DEPS) $(BINFOLDER)tail.o $(SBASE_UTIL_TAIL) $(LIBUTF_OBJS)
$(BINFOLDER)true.nxe:      $(DYN_DEPS) $(BINFOLDER)true.o
$(BINFOLDER)false.nxe:     $(DYN_DEPS) $(BINFOLDER)false.o
$(BINFOLDER)env.nxe:       $(DYN_DEPS) $(BINFOLDER)env.o $(SBASE_UTIL_ENV)
$(BINFOLDER)basename.nxe:  $(DYN_DEPS) $(BINFOLDER)basename.o $(SBASE_UTIL_BASENAME)
$(BINFOLDER)dirname.nxe:   $(DYN_DEPS) $(BINFOLDER)dirname.o $(SBASE_UTIL_DIRNAME)
$(BINFOLDER)sigtest.nxe:   $(DYN_DEPS) $(BINFOLDER)sigtest.o
$(BINFOLDER)crashtest.nxe: $(DYN_DEPS) $(BINFOLDER)crashtest.o
$(BINFOLDER)usbstorm.nxe:  $(DYN_DEPS) $(BINFOLDER)usbstorm.o
$(BINFOLDER)fputorture.nxe: $(DYN_DEPS) $(BINFOLDER)fputorture.o
$(BINFOLDER)socktest.nxe:  $(DYN_DEPS) $(BINFOLDER)socktest.o
$(BINFOLDER)pingtest.nxe:  $(DYN_DEPS) $(BINFOLDER)pingtest.o
$(BINFOLDER)nettest.nxe:   $(DYN_DEPS) $(BINFOLDER)nettest.o
$(BINFOLDER)unixtest.nxe:  $(DYN_DEPS) $(BINFOLDER)unixtest.o
$(BINFOLDER)tcpsrv.nxe:    $(DYN_DEPS) $(BINFOLDER)tcpsrv.o
$(BINFOLDER)drmtest.nxe:   $(DYN_DEPS) $(BINFOLDER)drmtest.o
$(BINFOLDER)i915test.nxe:  $(DYN_DEPS) $(BINFOLDER)i915test.o
$(BINFOLDER)glpix.nxe:     $(DYN_DEPS) $(BINFOLDER)glpix.o
$(BINFOLDER)nanologin.nxe: $(DYN_DEPS) $(BINFOLDER)nanologin.o
$(BINFOLDER)greeter.nxe:   $(DYN_DEPS) $(BINFOLDER)greeter.o
$(BINFOLDER)dhcpcfg.nxe:   $(DYN_DEPS) $(BINFOLDER)dhcpcfg.o
$(BINFOLDER)randhex.nxe:   $(DYN_DEPS) $(BINFOLDER)randhex.o
$(BINFOLDER)fbtest.nxe:    $(DYN_DEPS) $(BINFOLDER)fbtest.o
$(BINFOLDER)timetest.nxe:  $(DYN_DEPS) $(BINFOLDER)timetest.o
$(BINFOLDER)brktest.nxe:   $(DYN_DEPS) $(BINFOLDER)brktest.o
$(BINFOLDER)envtest.nxe:   $(DYN_DEPS) $(BINFOLDER)envtest.o
$(BINFOLDER)mmaptest.nxe:  $(DYN_DEPS) $(BINFOLDER)mmaptest.o
$(BINFOLDER)inputtest.nxe: $(DYN_DEPS) $(BINFOLDER)inputtest.o
$(BINFOLDER)mousetest.nxe: $(DYN_DEPS) $(BINFOLDER)mousetest.o
$(BINFOLDER)fstest.nxe:    $(DYN_DEPS) $(BINFOLDER)fstest.o
$(BINFOLDER)pipetest.nxe:  $(DYN_DEPS) $(BINFOLDER)pipetest.o
$(BINFOLDER)forkmany.nxe:  $(DYN_DEPS) $(BINFOLDER)forkmany.o
$(BINFOLDER)orphan.nxe:    $(DYN_DEPS) $(BINFOLDER)orphan.o
$(BINFOLDER)clonetest.nxe: $(DYN_DEPS) $(BINFOLDER)clonetest.o $(BINFOLDER)clone_helper.o
$(BINFOLDER)errnotest.nxe: $(DYN_DEPS) $(BINFOLDER)errnotest.o
$(BINFOLDER)malloctest.nxe: $(DYN_DEPS) $(BINFOLDER)malloctest.o
$(BINFOLDER)eventfdtest.nxe: $(DYN_DEPS) $(BINFOLDER)eventfdtest.o
$(BINFOLDER)epolltest.nxe: $(DYN_DEPS) $(BINFOLDER)epolltest.o
$(BINFOLDER)mmapexectest.nxe: $(DYN_DEPS) $(BINFOLDER)mmapexectest.o
$(BINFOLDER)resvtest.nxe: $(DYN_DEPS) $(BINFOLDER)resvtest.o
$(BINFOLDER)tlstest.nxe: $(DYN_DEPS) $(BINFOLDER)tlstest.o
$(BINFOLDER)mallocstorm.nxe: $(DYN_DEPS) $(BINFOLDER)mallocstorm.o
$(BINFOLDER)procselftest.nxe: $(DYN_DEPS) $(BINFOLDER)procselftest.o
$(BINFOLDER)shmdualtest.nxe: $(DYN_DEPS) $(BINFOLDER)shmdualtest.o
$(BINFOLDER)memfdtest.nxe: $(DYN_DEPS) $(BINFOLDER)memfdtest.o
$(BINFOLDER)pthrtest.nxe: $(DYN_DEPS) $(BINFOLDER)pthrtest.o
$(BINFOLDER)pthrstress.nxe: $(DYN_DEPS) $(BINFOLDER)pthrstress.o
$(BINFOLDER)futexpp.nxe:   $(DYN_DEPS) $(BINFOLDER)futexpp.o
$(BINFOLDER)pfract.nxe:    $(DYN_DEPS) $(BINFOLDER)pfract.o
$(BINFOLDER)smptorture.nxe: $(DYN_DEPS) $(BINFOLDER)smptorture.o
$(BINFOLDER)nettorture.nxe: $(DYN_DEPS) $(BINFOLDER)nettorture.o
$(BINFOLDER)ptytest.nxe:   $(DYN_DEPS) $(BINFOLDER)ptytest.o
$(BINFOLDER)nterm.nxe:     $(DYN_DEPS) $(BINFOLDER)nterm.o $(BINFOLDER)vt.o $(BINFOLDER)vtfont.o
# nanowm: the compositor (statically links the pure cores + gfx) and the notepad demo client
# (statically links libnw + the shared codec/gfx). Both dynamic-link libc.ndl via DYN_DEPS.
$(BINFOLDER)nwm.nxe:       $(DYN_DEPS) $(BINFOLDER)nwm.o $(BINFOLDER)nwm_core.o $(BINFOLDER)nw_compose.o $(BINFOLDER)nwproto.o $(BINFOLDER)nw_gfx.o $(BINFOLDER)vtfont.o $(BINFOLDER)nwfont.o $(BINFOLDER)stb_impl.o $(BINFOLDER)nwui_png.o $(BINFOLDER)nw_backdrop.o $(BINFOLDER)nw_settings.o
# notepad (the Notepad) is now a pure toolkit client like form/nwexp: --need libnwui.ndl pulls
# the whole chain libnwui->libnw->libc via the recursive loader.
$(BINFOLDER)notepad.nxe: $(DYN_GLUE) $(BINFOLDER)notepad.o $(BINFOLDER)libnwui.ndl.a $(BINFOLDER)libc.ndl.a $(BINFOLDER)libnwui.ndl $(BINFOLDER)libnw.ndl $(BINFOLDER)libc.ndl $(MKNX_TOOL)
	$(LD) -nostdlib -Wl,--emit-relocs -T $(USER_NX_LD) -o $(BINFOLDER)notepad.elf $(DYN_GLUE) $(BINFOLDER)notepad.o $(BINFOLDER)libnwui.ndl.a $(BINFOLDER)libc.ndl.a -lgcc
	$(MKNX_TOOL) $(BINFOLDER)notepad.elf $@ --need libnwui.ndl
# form uses ONLY the toolkit (+ libc for snprintf/exit); --need libnwui.ndl pulls the whole
# chain libnwui->libnw->libc via the recursive loader.
$(BINFOLDER)form.nxe: $(DYN_GLUE) $(BINFOLDER)form.o $(BINFOLDER)libnwui.ndl.a $(BINFOLDER)libc.ndl.a $(BINFOLDER)libnwui.ndl $(BINFOLDER)libnw.ndl $(BINFOLDER)libc.ndl $(MKNX_TOOL)
	$(LD) -nostdlib -Wl,--emit-relocs -T $(USER_NX_LD) -o $(BINFOLDER)form.elf $(DYN_GLUE) $(BINFOLDER)form.o $(BINFOLDER)libnwui.ndl.a $(BINFOLDER)libc.ndl.a -lgcc
	$(MKNX_TOOL) $(BINFOLDER)form.elf $@ --need libnwui.ndl
# nwexp: the file explorer — same toolkit-only chain as form (--need libnwui.ndl).
$(BINFOLDER)nwexp.nxe: $(DYN_GLUE) $(BINFOLDER)nwexp.o $(BINFOLDER)libnwui.ndl.a $(BINFOLDER)libc.ndl.a $(BINFOLDER)libnwui.ndl $(BINFOLDER)libnw.ndl $(BINFOLDER)libc.ndl $(MKNX_TOOL)
	$(LD) -nostdlib -Wl,--emit-relocs -T $(USER_NX_LD) -o $(BINFOLDER)nwexp.elf $(DYN_GLUE) $(BINFOLDER)nwexp.o $(BINFOLDER)libnwui.ndl.a $(BINFOLDER)libc.ndl.a -lgcc
	$(MKNX_TOOL) $(BINFOLDER)nwexp.elf $@ --need libnwui.ndl
# settings: the Settings demo — toolkit-only chain like form/nwexp.
$(BINFOLDER)settings.nxe: $(DYN_GLUE) $(BINFOLDER)settings.o $(BINFOLDER)nw_settings.o $(BINFOLDER)libnwui.ndl.a $(BINFOLDER)libc.ndl.a $(BINFOLDER)libnwui.ndl $(BINFOLDER)libnw.ndl $(BINFOLDER)libc.ndl $(MKNX_TOOL)
	$(LD) -nostdlib -Wl,--emit-relocs -T $(USER_NX_LD) -o $(BINFOLDER)settings.elf $(DYN_GLUE) $(BINFOLDER)settings.o $(BINFOLDER)nw_settings.o $(BINFOLDER)libnwui.ndl.a $(BINFOLDER)libc.ndl.a -lgcc
	$(MKNX_TOOL) $(BINFOLDER)settings.elf $@ --need libnwui.ndl
# about: "About This Computer" — toolkit-only chain like settings.
$(BINFOLDER)about.nxe: $(DYN_GLUE) $(BINFOLDER)about.o $(BINFOLDER)libnwui.ndl.a $(BINFOLDER)libc.ndl.a $(BINFOLDER)libnwui.ndl $(BINFOLDER)libnw.ndl $(BINFOLDER)libc.ndl $(MKNX_TOOL)
	$(LD) -nostdlib -Wl,--emit-relocs -T $(USER_NX_LD) -o $(BINFOLDER)about.elf $(DYN_GLUE) $(BINFOLDER)about.o $(BINFOLDER)libnwui.ndl.a $(BINFOLDER)libc.ndl.a -lgcc
	$(MKNX_TOOL) $(BINFOLDER)about.elf $@ --need libnwui.ndl
# nwbench: the GUI rendering benchmark (FPS meter over an animated widget scene) — toolkit-only chain.
$(BINFOLDER)nwbench.nxe: $(DYN_GLUE) $(BINFOLDER)nwbench.o $(BINFOLDER)libnwui.ndl.a $(BINFOLDER)libc.ndl.a $(BINFOLDER)libnwui.ndl $(BINFOLDER)libnw.ndl $(BINFOLDER)libc.ndl $(MKNX_TOOL)
	$(LD) -nostdlib -Wl,--emit-relocs -T $(USER_NX_LD) -o $(BINFOLDER)nwbench.elf $(DYN_GLUE) $(BINFOLDER)nwbench.o $(BINFOLDER)libnwui.ndl.a $(BINFOLDER)libc.ndl.a -lgcc
	$(MKNX_TOOL) $(BINFOLDER)nwbench.elf $@ --need libnwui.ndl
# viewer: the image viewer — toolkit-only chain (reuses the libnwui PNG decoder + image widget).
$(BINFOLDER)viewer.nxe: $(DYN_GLUE) $(BINFOLDER)viewer.o $(BINFOLDER)libnwui.ndl.a $(BINFOLDER)libc.ndl.a $(BINFOLDER)libnwui.ndl $(BINFOLDER)libnw.ndl $(BINFOLDER)libc.ndl $(MKNX_TOOL)
	$(LD) -nostdlib -Wl,--emit-relocs -T $(USER_NX_LD) -o $(BINFOLDER)viewer.elf $(DYN_GLUE) $(BINFOLDER)viewer.o $(BINFOLDER)libnwui.ndl.a $(BINFOLDER)libc.ndl.a -lgcc
	$(MKNX_TOOL) $(BINFOLDER)viewer.elf $@ --need libnwui.ndl
# properties: the standalone system "Properties" / Get-Info window (toolkit-only chain).
$(BINFOLDER)properties.nxe: $(DYN_GLUE) $(BINFOLDER)properties.o $(BINFOLDER)libnwui.ndl.a $(BINFOLDER)libc.ndl.a $(BINFOLDER)libnwui.ndl $(BINFOLDER)libnw.ndl $(BINFOLDER)libc.ndl $(MKNX_TOOL)
	$(LD) -nostdlib -Wl,--emit-relocs -T $(USER_NX_LD) -o $(BINFOLDER)properties.elf $(DYN_GLUE) $(BINFOLDER)properties.o $(BINFOLDER)libnwui.ndl.a $(BINFOLDER)libc.ndl.a -lgcc
	$(MKNX_TOOL) $(BINFOLDER)properties.elf $@ --need libnwui.ndl
# terminal: the real windowed Terminal — a raw libnw client running nsh on a pty, with the shared
# VT engine (vt.o). --need libnw.ndl.
$(BINFOLDER)terminal.nxe: $(DYN_GLUE) $(BINFOLDER)terminal.o $(BINFOLDER)vt.o $(BINFOLDER)libnw.ndl.a $(BINFOLDER)libc.ndl.a $(BINFOLDER)libnw.ndl $(BINFOLDER)libc.ndl $(MKNX_TOOL)
	$(LD) -nostdlib -Wl,--emit-relocs -T $(USER_NX_LD) -o $(BINFOLDER)terminal.elf $(DYN_GLUE) $(BINFOLDER)terminal.o $(BINFOLDER)vt.o $(BINFOLDER)libnw.ndl.a $(BINFOLDER)libc.ndl.a -lgcc
	$(MKNX_TOOL) $(BINFOLDER)terminal.elf $@ --need libnw.ndl
# rsexp: the x86_64 Rust file explorer (the desktop's Files app). Rust staticlib for
# the x86_64-nanos target, depending on the reusable libnwui-rs bindings crate. Linked with the
# x64 glue + USER_NX_LD + MKNX_TOOL; --need libnwui.ndl pulls the toolkit + PNG/fs helpers.
#
# SQLite-backed recursive search lights up when bin/libsqlite.ndl.a exists (i.e. `make ARCH=x86_64
# sqlite` was run): the crate is built with --features sqlite and linked against libsqlite.ndl, so
# the search box queries an in-memory SQLite index of the whole subtree. Without the lib the
# explorer still builds (in-memory substring filter), so a stock image never depends on the port.
# Both the cargo feature and the link are decided at recipe time, so re-running after `make sqlite`
# picks SQLite up (the lib.rs edit also forces the cargo rebuild).
RSEXP_TARGET=user/rust/x86_64-nanos.json
RSEXP_LIB=user/rust/rsexp/target/x86_64-nanos/release/librsexp.a
$(BINFOLDER)rsexp.nxe: user/rust/rsexp/src/lib.rs user/rust/libnwui-rs/src/lib.rs user/rust/rsexp/Cargo.toml user/rust/libnwui-rs/Cargo.toml $(RSEXP_TARGET) $(DYN_GLUE) $(BINFOLDER)libnwui.ndl.a $(BINFOLDER)libc.ndl.a $(BINFOLDER)libnwui.ndl $(BINFOLDER)libnw.ndl $(BINFOLDER)libc.ndl $(MKNX_TOOL)
	@if [ -f $(BINFOLDER)libsqlite.ndl.a ]; then \
	   echo "rsexp: SQLite-backed search ENABLED (linking libsqlite.ndl)"; \
	   ( cd user/rust/rsexp && cargo build --features sqlite -Z build-std=core,alloc -Z json-target-spec --target ../x86_64-nanos.json --release ); \
	   $(LD) -nostdlib -Wl,--emit-relocs -T $(USER_NX_LD) -o $(BINFOLDER)rsexp.elf $(DYN_GLUE) $(RSEXP_LIB) $(BINFOLDER)libnwui.ndl.a $(BINFOLDER)libsqlite.ndl.a $(BINFOLDER)libc.ndl.a -lgcc; \
	   $(MKNX_TOOL) $(BINFOLDER)rsexp.elf $@ --need libnwui.ndl --need libsqlite.ndl; \
	 else \
	   echo "rsexp: libsqlite.ndl.a absent -> in-memory filter (run 'make ARCH=x86_64 sqlite' to enable SQLite search)"; \
	   ( cd user/rust/rsexp && cargo build -Z build-std=core,alloc -Z json-target-spec --target ../x86_64-nanos.json --release ); \
	   $(LD) -nostdlib -Wl,--emit-relocs -T $(USER_NX_LD) -o $(BINFOLDER)rsexp.elf $(DYN_GLUE) $(RSEXP_LIB) $(BINFOLDER)libnwui.ndl.a $(BINFOLDER)libc.ndl.a -lgcc; \
	   $(MKNX_TOOL) $(BINFOLDER)rsexp.elf $@ --need libnwui.ndl; \
	 fi
$(BINFOLDER)tuitest.nxe:   $(DYN_DEPS) $(BINFOLDER)tuitest.o
$(BINFOLDER)racetest.nxe:  $(DYN_DEPS) $(BINFOLDER)racetest.o

# ---- Stage 3: the shared C library libc.ndl + its import library ----
# libc.ndl bundles picolibc + the syscall/cwd porting glue into ONE relocatable shared
# library exporting the C API, linked at the .ndl preferred base (the loader relocates it
# per process). We do NOT --whole-archive picolibc: that would pull its own sbrk/signal
# (clashing with our kernel-backed glue) and objects needing unimplemented syscalls
# (getentropy/sigprocmask). Instead our glue overrides those, and we export-all the rest of
# picolibc via the generated --undefined list below (see the libc.elf rule).
LIBC_GLUE_OBJS=$(BINFOLDER)syscalls.o $(BINFOLDER)cwd.o $(BINFOLDER)sigtramp.o $(BINFOLDER)termios.o \
  $(BINFOLDER)dirent.o $(BINFOLDER)pwd_grp.o $(BINFOLDER)grp_shadow.o $(BINFOLDER)posixstubs.o $(BINFOLDER)sockets.o $(BINFOLDER)resolv.o \
  $(BINFOLDER)resolv_parse.o $(BINFOLDER)stdio_ext.o $(BINFOLDER)ptyutil.o $(BINFOLDER)ifname.o \
  $(BINFOLDER)crypt.o $(BINFOLDER)tls.o $(BINFOLDER)retarget_lock.o $(BINFOLDER)getdelim.o $(LIBC_PTHREAD_OBJS)
# Phase 4: the vendored musl pthread internals (user/libc-glue/pthread/, musl 1.2.5),
# adapted to NanOS syscalls (int 0x80, Linux i386 numbers) + the picolibc TCB. Task 4.1
# brings in only the futex/clone/TLS primitives (no pthread_create yet); they link into
# libc.ndl. The vendored .c/.s files are built by the dedicated user/libc-glue/pthread rules
# below (extra -I for the musl headers + the weak_alias/hidden force-include).
LIBC_PTHREAD_OBJS=$(BINFOLDER)__wait.o $(BINFOLDER)__timedwait.o $(BINFOLDER)__lock.o \
  $(BINFOLDER)pthread_self.o $(BINFOLDER)default_attr.o $(BINFOLDER)nanos_glue.o \
  $(BINFOLDER)clone.o $(BINFOLDER)__set_thread_area.o $(BINFOLDER)nx_tls.o \
  $(BINFOLDER)pthread_create.o $(BINFOLDER)pthread_join.o $(BINFOLDER)pthread_detach.o \
  $(BINFOLDER)pthread_cancel.o $(BINFOLDER)cancel_impl.o \
  $(BINFOLDER)pthread_attr.o \
  $(BINFOLDER)pthread_mutex_lock.o $(BINFOLDER)pthread_mutex_unlock.o \
  $(BINFOLDER)pthread_mutex_trylock.o $(BINFOLDER)pthread_mutex_timedlock.o \
  $(BINFOLDER)pthread_mutex_init.o $(BINFOLDER)pthread_mutex_destroy.o \
  $(BINFOLDER)pthread_mutexattr_init.o $(BINFOLDER)pthread_mutexattr_destroy.o \
  $(BINFOLDER)pthread_mutexattr_settype.o $(BINFOLDER)vmlock.o \
  $(BINFOLDER)pthread_cond_wait.o $(BINFOLDER)pthread_cond_timedwait.o \
  $(BINFOLDER)pthread_cond_signal.o $(BINFOLDER)pthread_cond_broadcast.o \
  $(BINFOLDER)pthread_cond_init.o $(BINFOLDER)pthread_cond_destroy.o \
  $(BINFOLDER)pthread_rwlock_rdlock.o $(BINFOLDER)pthread_rwlock_tryrdlock.o \
  $(BINFOLDER)pthread_rwlock_timedrdlock.o $(BINFOLDER)pthread_rwlock_wrlock.o \
  $(BINFOLDER)pthread_rwlock_trywrlock.o $(BINFOLDER)pthread_rwlock_timedwrlock.o \
  $(BINFOLDER)pthread_rwlock_unlock.o $(BINFOLDER)pthread_rwlock_init.o \
  $(BINFOLDER)pthread_rwlock_destroy.o \
  $(BINFOLDER)pthread_barrier_wait.o $(BINFOLDER)pthread_barrier_init.o \
  $(BINFOLDER)pthread_barrier_destroy.o \
  $(BINFOLDER)pthread_spin_init.o $(BINFOLDER)pthread_spin_destroy.o \
  $(BINFOLDER)pthread_spin_lock.o $(BINFOLDER)pthread_spin_trylock.o \
  $(BINFOLDER)pthread_spin_unlock.o \
  $(BINFOLDER)pthread_once.o $(BINFOLDER)pthread_cleanup_push.o \
  $(BINFOLDER)pthread_key_create.o $(BINFOLDER)pthread_getspecific.o \
  $(BINFOLDER)pthread_setspecific.o \
  $(BINFOLDER)sem_init.o $(BINFOLDER)sem_destroy.o $(BINFOLDER)sem_wait.o \
  $(BINFOLDER)sem_trywait.o $(BINFOLDER)sem_timedwait.o $(BINFOLDER)sem_post.o \
  $(BINFOLDER)sem_getvalue.o $(BINFOLDER)sem_named.o
# libc.ndl is a COMPLETE C library: export every public picolibc function EXCEPT the handful
# our glue overrides (sbrk/signal/setenv/...). We force-undefine the whole picolibc surface
# (minus glue) so the linker pulls it in; because these are --undefined refs (not
# --whole-archive), a glue-defined symbol satisfies its ref and picolibc's conflicting
# version is never pulled. This lets ported programs (bash) detect/link any libc function
# instead of falling back to broken K&R paths. Depends on Makefile so the rule re-runs.
$(BINFOLDER)libc.elf: $(BINFOLDER)nxhdr.o $(LIBC_GLUE_OBJS) Makefile
	$(CROSS)nm $(LIBC_GLUE_OBJS) | awk '$$2=="T"{print $$3}' | sort -u > $(BINFOLDER)glue.syms
	$(CROSS)nm $(PICOLIBC)/lib/libc.a | awk '$$2=="T"{print $$3}' | sort -u > $(BINFOLDER)pico.syms
	comm -23 $(BINFOLDER)pico.syms $(BINFOLDER)glue.syms | sed 's/^/-Wl,--undefined=/' > $(BINFOLDER)libc.undef
	$(LD) -nostdlib -Wl,--emit-relocs -T user/dll.ld -o $@ $(BINFOLDER)nxhdr.o \
	  $(LIBC_GLUE_OBJS) @$(BINFOLDER)libc.undef -L$(PICOLIBC)/lib -lc -lgcc
$(BINFOLDER)libc.ndl: $(BINFOLDER)libc.elf $(MKNX_TOOL)
	$(MKNX_TOOL) $(BINFOLDER)libc.elf $@ --dll --export-all

# Import library libc.ndl.a: an ARCHIVE with ONE member per export — a `name: jmp
# [__imp_name]` thunk + IAT slot for each function, a slot-only member for each data symbol
# (stdout/errno, reached via nx-dllimport.h). A program links the archive instead of static
# picolibc; the linker pulls ONLY referenced members, so the program imports just what it
# uses (not all ~130 symbols). mknx writes one .s per symbol into a dir; we assemble + ar.
$(BINFOLDER)libc.ndl.a: $(BINFOLDER)libc.elf $(MKNX_TOOL)
	rm -rf $(BINFOLDER)libimp && mkdir -p $(BINFOLDER)libimp
	$(MKNX_TOOL) $(BINFOLDER)libc.elf $(BINFOLDER)libimp --implib --export-all --soname libc.ndl
	for f in $(BINFOLDER)libimp/*.s; do nasm -f $(ASM_FMT) "$$f" -o "$${f%.s}.o"; done
	rm -f $@ && ar rcs $@ $(BINFOLDER)libimp/*.o

# ---- libnw.ndl: the shared window-client library (the user32/gdi32 of nanowm) ----
# A real shared library (like greet.ndl/libc.ndl): the protocol codec + gfx + client API in
# ONE relocatable module, exporting the nw_* API. It USES libc (malloc/read/write/poll), so
# it links the libc import library and declares `--need libc.ndl`; the recursive loader
# (kernel/DynLoader.cpp) pulls libc.ndl in automatically when a client loads libnw.ndl.
LIBNW_OBJS=$(BINFOLDER)libnw.o $(BINFOLDER)nwproto.o $(BINFOLDER)nw_gfx.o $(BINFOLDER)vtfont.o $(BINFOLDER)nwfont.o $(BINFOLDER)stb_impl.o
$(BINFOLDER)libnw.elf: $(BINFOLDER)nxhdr.o $(LIBNW_OBJS) $(BINFOLDER)libc.ndl.a
	$(LD) -nostdlib -Wl,--emit-relocs -T user/dll.ld -o $@ $(BINFOLDER)nxhdr.o $(LIBNW_OBJS) $(BINFOLDER)libc.ndl.a -lgcc
$(BINFOLDER)libnw.ndl: $(BINFOLDER)libnw.elf $(MKNX_TOOL)
	$(MKNX_TOOL) $(BINFOLDER)libnw.elf $@ --dll --export-all --need libc.ndl
# Import library libnw.ndl.a (clients link this; the nw_* thunks bind to libnw.ndl at load).
$(BINFOLDER)libnw.ndl.a: $(BINFOLDER)libnw.elf $(MKNX_TOOL)
	rm -rf $(BINFOLDER)libnwimp && mkdir -p $(BINFOLDER)libnwimp
	$(MKNX_TOOL) $(BINFOLDER)libnw.elf $(BINFOLDER)libnwimp --implib --export-all --soname libnw.ndl
	for f in $(BINFOLDER)libnwimp/*.s; do nasm -f $(ASM_FMT) "$$f" -o "$${f%.s}.o"; done
	rm -f $@ && ar rcs $@ $(BINFOLDER)libnwimp/*.o

# ---- libnwui.ndl: the UI toolkit (the comctl32 of nanowm) ----
# Composable widget tree + flex layout + paint, exporting the nwui_* API. Uses libnw (gfx) AND
# libc (malloc); links both import libraries and declares both needs. A client that --need
# libnwui.ndl gets the whole chain app->libnwui->libnw->libc via the recursive loader.
LIBNWUI_OBJS=$(BINFOLDER)nwui_core.o $(BINFOLDER)nwui_paint.o $(BINFOLDER)nwui.o $(BINFOLDER)nwui_png.o $(BINFOLDER)nwui_fs.o
$(BINFOLDER)libnwui.elf: $(BINFOLDER)nxhdr.o $(LIBNWUI_OBJS) $(BINFOLDER)libnw.ndl.a $(BINFOLDER)libc.ndl.a
	$(LD) -nostdlib -Wl,--emit-relocs -T user/dll.ld -o $@ $(BINFOLDER)nxhdr.o $(LIBNWUI_OBJS) $(BINFOLDER)libnw.ndl.a $(BINFOLDER)libc.ndl.a -lgcc
$(BINFOLDER)libnwui.ndl: $(BINFOLDER)libnwui.elf $(MKNX_TOOL)
	$(MKNX_TOOL) $(BINFOLDER)libnwui.elf $@ --dll --export-all --need libnw.ndl --need libc.ndl
$(BINFOLDER)libnwui.ndl.a: $(BINFOLDER)libnwui.elf $(MKNX_TOOL)
	rm -rf $(BINFOLDER)libnwuiimp && mkdir -p $(BINFOLDER)libnwuiimp
	$(MKNX_TOOL) $(BINFOLDER)libnwui.elf $(BINFOLDER)libnwuiimp --implib --export-all --soname libnwui.ndl
	for f in $(BINFOLDER)libnwuiimp/*.s; do nasm -f $(ASM_FMT) "$$f" -o "$${f%.s}.o"; done
	rm -f $@ && ar rcs $@ $(BINFOLDER)libnwuiimp/*.o

# All programs + shared libraries (init -> /nanos/core, the rest -> /nanos/bin, libs -> /nanos/lib).

# x86_64 in-tree userland subset (Plan 10 Task 7): the shell + the sbase coreutils + init,
# all dynamically linked against the 64-bit libc.ndl (the same dynamic path as i686, via the
# MKNX_TOOL/USER_NX_LD). The full program set (nanowm, Doom, the
# pthread/net stress tools) is NOT built here — those are later ports; this is the first
# interactive 64-bit milestone (a working shell + ls/cat). init goes to /nanos/core, the
# rest to /nanos/bin (see _image64). free is a system util like the coreutils.
X64_SYS_PROGS=nsh open nanosu cat ls mkdir rmdir pwd touch rm ln cp mv chmod wc head tail true false env basename dirname free chsh pfract pthrstress futexpp smptorture nettorture usbstorm fputorture drmtest glpix i915test malloctest eventfdtest epolltest mmapexectest resvtest tlstest mallocstorm procselftest shmdualtest memfdtest
# nanowm compositor (nwm) is a system GUI program; the NetSurf libnsfb backend (and future GUI
# clients) link the libnw/libnwui import libs at load, so those .ndl ship to /nanos/lib too.
X64_GUI_PROGS=nwm greeter
# nanowm desktop client apps. nwm spawns them by absolute path from /disks/main/apps/<name>/<name>.nxe
# (see NWEXP_PATH etc. in user/nwm/nwm.c), so — unlike the compositor — they install as /apps bundles
# (+ a /bin symlink), NOT into /nanos/bin.
X64_GUI_APPS=rsexp settings about notepad viewer properties form terminal nwbench
X64_GUI_LIBS=libnw.ndl libnwui.ndl
X64_USER_PROGS=init $(X64_SYS_PROGS) $(X64_GUI_PROGS) $(X64_GUI_APPS)
_userland64: $(addprefix $(BINFOLDER),$(addsuffix .nxe,$(X64_USER_PROGS))) $(BINFOLDER)libc.ndl $(addprefix $(BINFOLDER),$(X64_GUI_LIBS)) $(BINFOLDER)dhcpcfg.nxe

# ----------------------------------------------------------------------------
# Kernel modules (nkext): loadable drivers built SEPARATELY from kernel.bin, shipped to
# /nanos/kext and loaded at boot (kernel/KextLoader). A kext is an NxFormat module (like a
# .ndl) but its imports resolve to the KERNEL export table (KernelExports), not libc.ndl. Kext
# code is ring-0 freestanding C++ (no picolibc); -Iinclude gives the freestanding string.h and
# we deliberately omit -Ilib so the macOS case-insensitivity trap can't bite when building in
# /src. (See docs/filesystem.md: /nanos/kext.)
# Kexts run in ring 0 with the KERNEL ABI, so they take the same machine-dependent codegen
# flags as the kernel ($(KARCHFLAGS) = -mno-red-zone -mno-sse -mno-mmx -mno-80387 — a
# ring-0 module must not touch SSE/x87 or the red zone across interrupts). $(CXX)/$(LD)
# are already the cross toolchain ($(CROSS)gcc).
KEXT_CFLAGS=-ffreestanding -nostdlib -nostdinc++ --no-exceptions --no-rtti \
  -fno-sized-deallocation -fno-leading-underscore -fno-pic -fno-stack-protector \
  -Iarch/include -Ikernel -Idrivers -Iinclude $(KARCHFLAGS) $(KOPTFLAGS)

# Kext format knobs: ELF64 (.nxlib.kernel slots are 8-byte `dq`, RIP-relative
# `jmp [rel ...]` thunks, base < 2 GiB via kext64.ld), packed by mknx64 (v4 / R_X86_64_*).
KEXT_LD=kext/kext64.ld
KIMP_BITS=64
KIMP_DW=dq
KIMP_ALIGN=8
KIMP_REL=rel

# Kernel import library for kexts, GENERATED from kexports.def (single source of truth): a
# module links these thunks/slots (__imp_knx_* in section .nxlib.kernel), which mknx turns
# into "kernel"-scoped imports the loader binds against KernelExports.
$(BINFOLDER)kimports.S: kernel/kexports.def
	@mkdir -p $(BINFOLDER)
	@awk -v bits=$(KIMP_BITS) -v dw=$(KIMP_DW) -v algn=$(KIMP_ALIGN) -v rel="$(KIMP_REL)" \
	  'BEGIN{print "[BITS " bits "]"; print "section .nxlib.kernel progbits alloc write align=" algn} \
	  /^KX\(/{ n=$$0; sub(/^KX\(/,"",n); sub(/\).*/,"",n); k[++c]=n; print "[GLOBAL __imp_" n "]"; print "__imp_" n ": " dw " 0" } \
	  END{ print "section .text"; for(i=1;i<=c;i++){ print "[GLOBAL " k[i] "]"; r=(rel==""?"":rel " "); print k[i] ": jmp [" r "__imp_" k[i] "]" } }' $< > $@
$(BINFOLDER)kimports.o: $(BINFOLDER)kimports.S
	nasm -f $(ASM_FMT) $< -o $@

# Kext source objects (NOT in kernel SOURCES; never linked into kernel.bin).
$(BINFOLDER)%.o: kext/%.cpp
	@mkdir -p $(BINFOLDER)
	$(CXX) $(KEXT_CFLAGS) -MMD -MP -c $< -o $@
$(BINFOLDER)%.o: kext/mouse/%.cpp
	@mkdir -p $(BINFOLDER)
	$(CXX) $(KEXT_CFLAGS) -MMD -MP -c $< -o $@
$(BINFOLDER)%.o: kext/kbd/%.cpp
	@mkdir -p $(BINFOLDER)
	$(CXX) $(KEXT_CFLAGS) -MMD -MP -c $< -o $@
$(BINFOLDER)%.o: kext/e1000/%.cpp
	@mkdir -p $(BINFOLDER)
	$(CXX) $(KEXT_CFLAGS) -MMD -MP -c $< -o $@
$(BINFOLDER)%.o: kext/i219/%.cpp
	@mkdir -p $(BINFOLDER)
	$(CXX) $(KEXT_CFLAGS) -Ikext/e1000 -MMD -MP -c $< -o $@   # -Ikext/e1000: i219_phy.cpp needs e1000_core.h

# LinuxKPI shim (C) + the virtio_gpu module (x86_64 only). Same ring-0 codegen as a kext
# ($(KEXT_CFLAGS)) PLUS the Linux compat include tree (-Ilinuxkpi/include so <linux/...>
# resolves to the shim), __KERNEL__, and force-included autoconf.h + compat.h. Compiled as
# C (gnu11) by $(CXX)=$(CROSS)gcc (extension-driven).
LINUXKPI_CFLAGS=$(KEXT_CFLAGS) -std=gnu11 -D__KERNEL__ -Ilinuxkpi -Ilinuxkpi/include \
  -include linuxkpi/autoconf.h -include linuxkpi/compat.h \
  -Wno-unused -Wno-unused-parameter -Wno-implicit-fallthrough \
  -Wno-incompatible-pointer-types -Wno-attributes -Wno-builtin-declaration-mismatch
# Vendored-Linux include tree (virtio/DRM subsystem headers) + the module dir. The shim's
# -Ilinuxkpi/include comes FIRST so <linux/foo.h> kernel-API headers resolve to the shim;
# virtio/drm-specific headers fall through to the vendored tree.
LINUXKPI_VINC=-Iexternal/linux-6.12/include -Iexternal/linux-6.12/include/uapi -Ikext/virtio_gpu
$(BINFOLDER)%.o: linuxkpi/%.c
	@mkdir -p $(BINFOLDER)
	$(CXX) $(LINUXKPI_CFLAGS) $(LINUXKPI_VINC) -MMD -MP -c $< -o $@
$(BINFOLDER)%.o: kext/virtio_gpu/%.c
	@mkdir -p $(BINFOLDER)
	$(CXX) $(LINUXKPI_CFLAGS) $(DRM_VINC) -Iexternal/linux-6.12/drivers/gpu/drm/virtio -MMD -MP -c $< -o $@
# Vendored Linux virtio core (compiled UNMODIFIED against the shim).
$(BINFOLDER)%.o: external/linux-6.12/drivers/virtio/%.c
	@mkdir -p $(BINFOLDER)
	$(CXX) $(LINUXKPI_CFLAGS) $(LINUXKPI_VINC) -MMD -MP -c $< -o $@
# Vendored Linux DRM/KMS core + virtio_gpu DRM driver + lib helpers (compiled UNMODIFIED
# against the shim). DRM files also need the drm subsystem's own dir on the include path.
DRM_VINC=$(LINUXKPI_VINC) -Iexternal/linux-6.12/drivers/gpu/drm
$(BINFOLDER)%.o: external/linux-6.12/drivers/gpu/drm/%.c
	@mkdir -p $(BINFOLDER)
	$(CXX) $(LINUXKPI_CFLAGS) $(DRM_VINC) -MMD -MP -c $< -o $@
$(BINFOLDER)%.o: external/linux-6.12/drivers/gpu/drm/virtio/%.c
	@mkdir -p $(BINFOLDER)
	$(CXX) $(LINUXKPI_CFLAGS) $(DRM_VINC) -MMD -MP -c $< -o $@
$(BINFOLDER)%.o: external/linux-6.12/lib/%.c
	@mkdir -p $(BINFOLDER)
	$(CXX) $(LINUXKPI_CFLAGS) $(LINUXKPI_VINC) -MMD -MP -c $< -o $@

# i915 (Task 5 of the Dell GPU plan): the vendored driver's own tree adds the module root to the
# include path (`subdir-ccflags -I$(src)`) and defines I915. Flags reuse the kext build's
# LINUXKPI_CFLAGS + DRM include tree so the campaign and the eventual kext link never drift.
I915_SRC=external/linux-6.12/drivers/gpu/drm/i915
I915_VINC=$(DRM_VINC) -I$(I915_SRC) -DI915
# i915 REQUIRES optimization: IS_PLATFORM/IS_SUBPLATFORM etc. do BUILD_BUG_ON(!__builtin_constant_p(p)),
# which only folds to a constant under the optimizer — the real kernel always builds i915 at -O2. The
# kext core defaults to -O0 (KOPTFLAGS empty; kernel -O2 is blocked on an unrelated ext UB), so we opt
# in -O2 for i915 objects specifically. Kept on both the probe and the object rule so they never drift.
I915_OPT=-O2
$(BINFOLDER)i915/%.o: $(I915_SRC)/%.c
	@mkdir -p $(@D)
	$(CXX) $(LINUXKPI_CFLAGS) $(I915_OPT) $(I915_VINC) -MMD -MP -c $< -o $@
# Compile-campaign scorer (in-container half): how many of the 276 vendored i915 objects build
# against the shim. scripts/build-i915.sh keeps clean/total + the cluster map. The host-side
# `i915-probe` wrapper (which runs this in Docker) lives in the HOST section next to test64.
_i915_probe:
	CC="$(CXX)" CFLAGS="$(LINUXKPI_CFLAGS) $(I915_OPT) $(I915_VINC)" I915_BUILD_OUT="$(BINFOLDER)i915-build" bash scripts/build-i915.sh

# Per-kext link: nxhdr placeholder + generated kernel import stub + kext runtime + objects,
# linked at the kext base with relocations kept (--emit-relocs), then mknx -> .nkext.
KEXT_GLUE=$(BINFOLDER)nxhdr.o $(BINFOLDER)kimports.o $(BINFOLDER)kext_rt.o
$(BINFOLDER)mouse.nkext: $(KEXT_GLUE) $(BINFOLDER)MouseDevice.o $(BINFOLDER)mouse_ps2.o $(MKNX_TOOL) $(KEXT_LD)
	$(LD) -nostdlib -Wl,--emit-relocs -T $(KEXT_LD) -o $(@:.nkext=.elf) \
	  $(KEXT_GLUE) $(BINFOLDER)MouseDevice.o $(BINFOLDER)mouse_ps2.o -lgcc
	$(MKNX_TOOL) $(@:.nkext=.elf) $@
$(BINFOLDER)kbd.nkext: $(KEXT_GLUE) $(BINFOLDER)kbd_ps2.o $(MKNX_TOOL) $(KEXT_LD)
	$(LD) -nostdlib -Wl,--emit-relocs -T $(KEXT_LD) -o $(@:.nkext=.elf) \
	  $(KEXT_GLUE) $(BINFOLDER)kbd_ps2.o -lgcc
	$(MKNX_TOOL) $(@:.nkext=.elf) $@
$(BINFOLDER)e1000.nkext: $(KEXT_GLUE) $(BINFOLDER)e1000.o $(BINFOLDER)e1000_core.o $(MKNX_TOOL) $(KEXT_LD)
	$(LD) -nostdlib -Wl,--emit-relocs -T $(KEXT_LD) -o $(@:.nkext=.elf) \
	  $(KEXT_GLUE) $(BINFOLDER)e1000.o $(BINFOLDER)e1000_core.o -lgcc
	$(MKNX_TOOL) $(@:.nkext=.elf) $@
$(BINFOLDER)e1000e.nkext: $(KEXT_GLUE) $(BINFOLDER)e1000e.o $(BINFOLDER)e1000_core.o $(MKNX_TOOL) $(KEXT_LD)
	$(LD) -nostdlib -Wl,--emit-relocs -T $(KEXT_LD) -o $(@:.nkext=.elf) \
	  $(KEXT_GLUE) $(BINFOLDER)e1000e.o $(BINFOLDER)e1000_core.o -lgcc
	$(MKNX_TOOL) $(@:.nkext=.elf) $@
$(BINFOLDER)i219.nkext: $(KEXT_GLUE) $(BINFOLDER)i219.o $(BINFOLDER)i219_phy.o $(BINFOLDER)e1000_core.o $(MKNX_TOOL) $(KEXT_LD)
	$(LD) -nostdlib -Wl,--emit-relocs -T $(KEXT_LD) -o $(@:.nkext=.elf) \
	  $(KEXT_GLUE) $(BINFOLDER)i219.o $(BINFOLDER)i219_phy.o $(BINFOLDER)e1000_core.o -lgcc
	$(MKNX_TOOL) $(@:.nkext=.elf) $@

# virtio_gpu module: the UNMODIFIED Linux 6.12 virtio_gpu DRM driver + DRM/KMS core + virtio
# core + lib helpers, all compiled against the LinuxKPI shim, linked with the shim runtime
# (kpi_*.o), the hand-built modern virtio-pci transport, and the kext bootstrap that runs the
# real virtio_gpu_probe(). This is the full DRM lift (no hand-written GPU protocol).
LINUXKPI_OBJS=$(BINFOLDER)kpi_slab.o $(BINFOLDER)kpi_print.o $(BINFOLDER)kpi_idr.o \
  $(BINFOLDER)kpi_sort.o $(BINFOLDER)kpi_time.o $(BINFOLDER)kpi_string.o \
  $(BINFOLDER)kpi_mm.o $(BINFOLDER)kpi_dma.o $(BINFOLDER)kpi_pci.o \
  $(BINFOLDER)kpi_sg.o $(BINFOLDER)kpi_fence.o $(BINFOLDER)kpi_misc.o \
  $(BINFOLDER)kpi_irq.o $(BINFOLDER)kpi_kthread.o $(BINFOLDER)kpi_rcu.o \
  $(BINFOLDER)kpi_firmware.o $(BINFOLDER)kpi_iomap.o $(BINFOLDER)kpi_shrinker.o
# Vendored Linux virtio core objects (built from external/linux-6.12 via the rules above).
VIRTIO_CORE_OBJS=$(BINFOLDER)virtio_ring.o $(BINFOLDER)virtio_pci_modern_dev.o
# Vendored Linux lib helpers (red-black trees, list sort).
DRM_LIB_OBJS=$(BINFOLDER)rbtree.o $(BINFOLDER)list_sort.o
# The UNMODIFIED virtio_gpu DRM driver (drivers/gpu/drm/virtio/*.c).
DRM_DRIVER_OBJS=$(BINFOLDER)virtgpu_drv.o $(BINFOLDER)virtgpu_kms.o $(BINFOLDER)virtgpu_gem.o \
  $(BINFOLDER)virtgpu_vram.o $(BINFOLDER)virtgpu_display.o $(BINFOLDER)virtgpu_vq.o \
  $(BINFOLDER)virtgpu_fence.o $(BINFOLDER)virtgpu_object.o $(BINFOLDER)virtgpu_plane.o \
  $(BINFOLDER)virtgpu_ioctl.o $(BINFOLDER)virtgpu_prime.o $(BINFOLDER)virtgpu_submit.o \
  $(BINFOLDER)virtgpu_debugfs.o $(BINFOLDER)virtgpu_trace_points.o
# The UNMODIFIED DRM/KMS core subset the driver pulls in (drivers/gpu/drm/*.c).
DRM_CORE_OBJS=$(BINFOLDER)drm_aperture.o $(BINFOLDER)drm_atomic_helper.o \
  $(BINFOLDER)drm_atomic_state_helper.o $(BINFOLDER)drm_atomic_uapi.o $(BINFOLDER)drm_atomic.o \
  $(BINFOLDER)drm_auth.o $(BINFOLDER)drm_blend.o $(BINFOLDER)drm_bridge.o $(BINFOLDER)drm_cache.o \
  $(BINFOLDER)drm_client_modeset.o $(BINFOLDER)drm_client.o $(BINFOLDER)drm_color_mgmt.o \
  $(BINFOLDER)drm_connector.o $(BINFOLDER)drm_crtc_helper.o $(BINFOLDER)drm_crtc.o \
  $(BINFOLDER)drm_damage_helper.o $(BINFOLDER)drm_displayid.o $(BINFOLDER)drm_drv.o \
  $(BINFOLDER)drm_dumb_buffers.o $(BINFOLDER)drm_edid.o $(BINFOLDER)drm_eld.o \
  $(BINFOLDER)drm_encoder.o $(BINFOLDER)drm_fb_dma_helper.o $(BINFOLDER)drm_file.o \
  $(BINFOLDER)drm_format_helper.o $(BINFOLDER)drm_fourcc.o $(BINFOLDER)drm_framebuffer.o \
  $(BINFOLDER)drm_gem_atomic_helper.o $(BINFOLDER)drm_gem_framebuffer_helper.o \
  $(BINFOLDER)drm_gem_shmem_helper.o $(BINFOLDER)drm_gem.o $(BINFOLDER)drm_ioctl.o \
  $(BINFOLDER)drm_kms_helper_common.o $(BINFOLDER)drm_lease.o $(BINFOLDER)drm_managed.o \
  $(BINFOLDER)drm_mm.o $(BINFOLDER)drm_mode_config.o $(BINFOLDER)drm_mode_object.o \
  $(BINFOLDER)drm_modes.o $(BINFOLDER)drm_modeset_helper.o $(BINFOLDER)drm_modeset_lock.o \
  $(BINFOLDER)drm_panel_orientation_quirks.o $(BINFOLDER)drm_pci.o $(BINFOLDER)drm_plane_helper.o \
  $(BINFOLDER)drm_plane.o $(BINFOLDER)drm_prime.o $(BINFOLDER)drm_print.o \
  $(BINFOLDER)drm_probe_helper.o $(BINFOLDER)drm_property.o $(BINFOLDER)drm_rect.o \
  $(BINFOLDER)drm_self_refresh_helper.o $(BINFOLDER)drm_simple_kms_helper.o \
  $(BINFOLDER)drm_syncobj.o $(BINFOLDER)drm_sysfs.o $(BINFOLDER)drm_trace_points.o \
  $(BINFOLDER)drm_vblank_work.o $(BINFOLDER)drm_vblank.o $(BINFOLDER)drm_vma_manager.o \
  $(BINFOLDER)drm_writeback.o
# The module: kext bootstrap + hand-built transport (port equivalent of virtio_pci_common.c).
VIRTIO_GPU_OBJS=$(BINFOLDER)virtio_gpu_drv_entry.o $(BINFOLDER)virtio_transport.o $(BINFOLDER)virtio_gpu_present.o $(BINFOLDER)virtio_gpu_drm_node.o
$(BINFOLDER)virtio_gpu.nkext: $(KEXT_GLUE) $(VIRTIO_GPU_OBJS) $(DRM_DRIVER_OBJS) $(DRM_CORE_OBJS) $(DRM_LIB_OBJS) $(VIRTIO_CORE_OBJS) $(LINUXKPI_OBJS) $(MKNX_TOOL) $(KEXT_LD)
	$(LD) -nostdlib -Wl,--emit-relocs -T $(KEXT_LD) -o $(@:.nkext=.elf) \
	  $(KEXT_GLUE) $(VIRTIO_GPU_OBJS) $(DRM_DRIVER_OBJS) $(DRM_CORE_OBJS) $(DRM_LIB_OBJS) $(VIRTIO_CORE_OBJS) $(LINUXKPI_OBJS) -lgcc
	$(MKNX_TOOL) $(@:.nkext=.elf) $@

# --- i915 LINK phase (Task 5 final step): the full unmodified 6.12 i915 driver -------------------
# All 276 objects compile (make i915-probe = 276/276). I915_OBJS is derived from the SAME reproducible
# list the probe scores (scripts/i915-objs.txt) so the two never drift. Each builds via the
# $(BINFOLDER)i915/%.o rule (at -O2, DRM_VINC includes).
I915_OBJS=$(patsubst %,$(BINFOLDER)i915/%.o,$(shell cat scripts/i915-objs.txt))
# Build-only phony to materialise every i915 .o (in-container half; host wrapper near i915-probe),
# used to scope the link's unresolved-symbol surface before the kext glue exists.
_i915-objs: $(I915_OBJS)
	@echo "i915: built $(words $(I915_OBJS)) objects"

# Link-phase SUPPORT objects: TTM core, DRM display (DP/HDCP/DSC) helpers, drm_buddy/mipi_dsi,
# dma-buf helpers, and the vendored non-DRM subsystems (hdmi/i2c-bit/intel-gtt) that i915 calls into.
# Built at -O2 with the DRM vendored-include set (no -DI915, no i915 src dir). Scored like i915-probe.
_link-support-probe:
	@mkdir -p bin/i915-link; clean=0; tot=0; \
	for c in $$(cat scripts/link-support-objs.txt); do \
	  tot=$$((tot+1)); flat=$$(echo $$c | sed 's|external/linux-6.12/||; s|/|_|g; s|\.c$$||'); \
	  if $(CXX) $(LINUXKPI_CFLAGS) -O2 $(DRM_VINC) -c external/linux-6.12/$$c -o bin/i915-link/$$flat.o 2> bin/i915-link/$$flat.err; then \
	    clean=$$((clean+1)); else echo "FAIL: $$c"; fi; \
	done; \
	echo "support clean/total: $$clean/$$tot"

# SUPPORT objects as real link inputs: one $(eval)'d rule per source in the scored list, so the
# flat bin/i915-link/<path_with_underscores>.o names map back to their slashed source paths (a
# plain pattern rule can't un-flatten). Same -O2 + DRM_VINC recipe the probe uses — never drifts.
LINK_SUPPORT_SRCS=$(shell cat scripts/link-support-objs.txt)
SUPPORT_OBJS=$(foreach s,$(LINK_SUPPORT_SRCS),bin/i915-link/$(subst /,_,$(s:.c=)).o)
define SUPPORT_OBJ_RULE
bin/i915-link/$(subst /,_,$(1:.c=)).o: external/linux-6.12/$(1)
	@mkdir -p bin/i915-link
	$$(CXX) $$(LINUXKPI_CFLAGS) -O2 $$(DRM_VINC) -c $$< -o $$@
endef
$(foreach s,$(LINK_SUPPORT_SRCS),$(eval $(call SUPPORT_OBJ_RULE,$(s))))

# The i915 kext glue (bootstrap + PCI-driver registration + legacy-GMCH stubs + stolen_res).
# Built with the DRM vendored-include set so <drm/intel/intel-gtt.h> resolves.
# Built with I915_VINC (adds -I$(I915_SRC) -DI915) so the glue can include i915_params.h and set
# i915_modparams (enable_guc / inject_probe_failure) type-safely — no shim header keys off -DI915,
# so the glue's view of every shared struct stays identical to the i915 objects it calls into.
# FORCE prereq + -DLKPI_GIT_REV: the entry stamps the running shim's git rev into the boot log so a
# Dell log unambiguously says WHICH build booted (a commit alone wouldn't retrigger this rule via the
# .c mtime, so recompile it every make — one tiny file). "-dirty" flags an uncommitted working tree.
$(BINFOLDER)i915_entry.o: kext/i915/i915_entry.c FORCE
	$(CXX) $(LINUXKPI_CFLAGS) $(I915_VINC) -DLKPI_GIT_REV='"$(shell git rev-parse --short HEAD 2>/dev/null)$(shell git diff --quiet 2>/dev/null || echo -dirty)"' -MMD -MP -c $< -o $@
# The desktop->panel mirror bridge (post-modeset present callback). Same vendored-include build as
# the entry glue so <linux/*> + lkpi_knx.h resolve identically.
$(BINFOLDER)i915_present.o: kext/i915/i915_present.c
	$(CXX) $(LINUXKPI_CFLAGS) $(I915_VINC) -MMD -MP -c $< -o $@
# The /dev/dri node dispatch (per-process drm_file + drm_ioctl + i915 mmap-offset resolution).
# Needs the i915 include set (i915_drv.h + gem object internals) AND -O2: i915_drv.h's
# IS_PLATFORM/IS_SUBPLATFORM BUILD_BUG_ON(__builtin_constant_p(...)) only folds under
# optimization — the same reason every vendored i915 object builds -O2.
$(BINFOLDER)i915_drm_node.o: kext/i915/i915_drm_node.c
	$(CXX) $(LINUXKPI_CFLAGS) -O2 $(I915_VINC) -MMD -MP -c $< -o $@
FORCE:
.PHONY: FORCE
I915_GLUE_OBJS=$(BINFOLDER)i915_entry.o $(BINFOLDER)i915_present.o $(BINFOLDER)i915_drm_node.o

# The link: kext bootstrap + i915 glue + all 276 i915 objects + the TTM/DRM-display SUPPORT set +
# the shared DRM core/lib + the LinuxKPI shim runtime. No virtio objects. Produces bin/i915.nkext.
$(BINFOLDER)i915.nkext: $(KEXT_GLUE) $(I915_GLUE_OBJS) $(I915_OBJS) $(SUPPORT_OBJS) $(DRM_CORE_OBJS) $(DRM_LIB_OBJS) $(LINUXKPI_OBJS) $(MKNX_TOOL) $(KEXT_LD)
	$(LD) -nostdlib -Wl,--emit-relocs -T $(KEXT_LD) -o $(@:.nkext=.elf) \
	  $(KEXT_GLUE) $(I915_GLUE_OBJS) $(I915_OBJS) $(SUPPORT_OBJS) $(DRM_CORE_OBJS) $(DRM_LIB_OBJS) $(LINUXKPI_OBJS) -lgcc
	$(MKNX_TOOL) $(@:.nkext=.elf) $@

# Track header dependencies for the vendored kext objects (all built with -MMD -MP). Without this,
# a shim-HEADER change (e.g. a linuxkpi/include/linux/*.h edit that turns a stub inline into a real
# out-of-line function) does NOT rebuild the i915/DRM/support objects that #include it — they keep
# the stale inlined stub and the link silently uses old behaviour on real hardware. (The kernel
# objects are covered by the -include at $(OBJECTS:.o=.d); these vendored groups were not.)
-include $(I915_OBJS:.o=.d) $(I915_GLUE_OBJS:.o=.d) $(SUPPORT_OBJS:.o=.d)
-include $(DRM_CORE_OBJS:.o=.d) $(DRM_LIB_OBJS:.o=.d) $(LINUXKPI_OBJS:.o=.d)

KEXTS=kbd mouse e1000 e1000e i219
# The LinuxKPI virtio_gpu + i915 modules are x86_64-only (vendored Linux source assumes 64-bit).
# i915 ships in the image but is a safe no-op unless armed via /nanos/config/i915 (=1): unarmed,
# its nkext_init returns before any DRM/i915 init, so it never double-inits DRM core against
# virtio_gpu and adds nothing to a normal boot. Armed (Dell bring-up), it runs the full driver.
KEXTS+= virtio_gpu i915
_kext: $(addprefix $(BINFOLDER),$(addsuffix .nkext,$(KEXTS)))

# Doom (doomgeneric). Old-C source needs -fcommon (GCC 10+ defaults to -fno-common, which
# breaks Doom's tentative globals) and warnings off; -DNORMALUNIX -DLINUX select the POSIX
# code paths; -lm for the renderer's trig/sqrt. Our platform layer (doomgeneric_nanos.c)
# replaces the shipped backends. Built as the `doom` program (staged by `make doom`).
DOOM_DIR=user/third_party/doomgeneric
DOOM_CFLAGS=-ffreestanding -isystem $(PICOLIBC)/include -iquote kernel -Iuser -Iuser/libc-glue/include -I$(DOOM_DIR) -D_DEFAULT_SOURCE -DNORMALUNIX -DLINUX -include user/libc-glue/compat-decls.h -include user/libc-glue/nx-dllimport.h -w -fcommon -fno-pic -fno-stack-protector $(USER_ARCHFLAGS) $(UOPTFLAGS)
DOOM_OBJS=$(patsubst $(DOOM_DIR)/%.c,$(BINFOLDER)%.o,$(wildcard $(DOOM_DIR)/*.c))

# Per-object rules (with -MMD header tracking) so only CHANGED Doom sources recompile
# — the old wildcard loop rebuilt all ~80 files on every `make run` (minutes under
# amd64 emulation). The link itself still runs each build (fast), picking up fresh glue.
$(BINFOLDER)%.o: $(DOOM_DIR)/%.c
	$(CXX) $(DOOM_CFLAGS) -MMD -MP -c $< -o $@
$(BINFOLDER)doomgeneric_nanos.o: user/doomgeneric_nanos.c
	$(CXX) $(DOOM_CFLAGS) -MMD -MP -c $< -o $@

# Doom links dynamically against libc.ndl (functions via the import library, math from the
# symbols force-included into libc.ndl — no static -lm). A trailing -lc is a FALLBACK that
# statically pulls only still-unresolved symbols: read-only const tables like _ctype_b
# (importing immutable data has no shared-state benefit, unlike stdout/errno). Functions are
# already resolved by the thunks, so their libc.a members are not pulled.
$(BINFOLDER)doom.nxe: $(DYN_GLUE) $(DOOM_OBJS) $(BINFOLDER)doomgeneric_nanos.o $(BINFOLDER)libc.ndl.a $(BINFOLDER)libc.ndl $(MKNX_TOOL)
	$(LD) -nostdlib -Wl,--emit-relocs -T $(USER_NX_LD) -o $(BINFOLDER)doom.elf $(DYN_GLUE) $(DOOM_OBJS) $(BINFOLDER)doomgeneric_nanos.o $(BINFOLDER)libc.ndl.a -L$(PICOLIBC)/lib -lc -lgcc
	$(MKNX_TOOL) $(BINFOLDER)doom.elf $(BINFOLDER)doom.nxe --need libc.ndl

# `make doom`: stage bin/doom.nxe. Doom is NOT in the minimal X64_USER_PROGS subset, so this
# target is the way to stage it; `make image64` then installs the /apps/doom bundle +
# /bin/doom.nxe symlink (only if bin/doom.nxe exists).
.PHONY: doom
doom: $(BINFOLDER)doom.nxe
	@echo "staged $(BINFOLDER)doom.nxe — run 'make image64' to install the /apps/doom bundle"

# Pull in all userland header-dependency files (.d), so a changed header recompiles only
# the objects that include it. Missing on a clean build -> everything compiles (correct).
-include $(wildcard $(BINFOLDER)*.d)

# ----------------------------------------------------------------------------
# Host-compiled test suite (doctest) + coverage gate.
# The storage stack is pure software, so we run it natively (host g++) against an
# in-memory RamBlockDevice. Memory/string come from libc (we do NOT link
# memory_manager/string_funcs); Console is stubbed by tests/host_shims.cpp.
# ----------------------------------------------------------------------------
HOST_CXX=g++
# AddressSpace + its paging headers: x86_64 4-level paging (the only arch since the i686
# retirement, docs/superpowers/plans/2026-07-13-i686-retirement.md).
ARCH_MM_INC=-Iarch/x86_64/mm
ARCH_ADDRSPACE=arch/x86_64/mm/AddressSpace.cpp
# Host include path: code dirs only, deliberately WITHOUT -Iinclude so that
# <string.h> resolves to libc (not the freestanding include/string.h).
HINCLUDES=-Iarch/include -Ikernel -Idrivers -Ifs -Imm -Ilib -Inet -Iusb -Iarch/x86_64/boot $(ARCH_MM_INC) -Iarch/x86_64/cpu -Ikext/mouse -Ikext/e1000 -Ikext/i219 -Ikext/virtio_gpu -Iuser/libnw -Iuser/nwm -Iuser/libnwui -Iuser/term -Iuser/third_party/stb -Iuser/libc-glue -Ilinuxkpi/include -Ilinuxkpi
# The host is LP64 (arm64/x86_64) but does not define __x86_64__, so force the v4 64-bit
# .nx format (nxaddr_t = uint64_t) across the whole host test build. This exercises the
# x86_64 loader path (R_X86_64_64 fixups, 8-byte IAT slots) and keeps every TU's view of
# NxFormat.h consistent (NxeLoader.cpp + its test must agree on the loadImage signature).
HOST_CXXFLAGS=-std=c++17 -O0 -g $(HINCLUDES) -Wall --coverage -DNX_FORCE64=1 -DNANOS_HOST_TEST=1
TEST_BIN=/tmp/nanos_tests
TEST_SRCS=$(wildcard tests/*.cpp)
# Modules under test (grown as layers are added). Header-only modules contribute
# coverage via the .h patterns below.
TEST_MODULES=drivers/RamBlockDevice.cpp drivers/DeviceManager.cpp drivers/Console.cpp fs/Vfs.cpp fs/ExtFilesystem.cpp fs/SynthFs.cpp fs/RamFs.cpp fs/ext/Crc32c.cpp fs/ext/BlockCache.cpp fs/ext/ExtCsum.cpp fs/ext/ExtAllocator.cpp fs/ext/Journal.cpp kernel/Syscall.cpp kernel/Cred.cpp kernel/NxeLoader.cpp kernel/KeyDecoder.cpp kernel/Scheduler.cpp kernel/Process.cpp kernel/Signal.cpp kernel/Futex.cpp kernel/Csprng.cpp kernel/Acpi.cpp kernel/Bkl.cpp lib/String.cpp
TEST_MODULES+= arch/x86_64/boot/MultibootMmap.cpp mm/FrameAllocator.cpp mm/Heap.cpp $(ARCH_ADDRSPACE)
TEST_MODULES+= arch/x86_64/cpu/lapic_x86_64.cpp   # pure-logic half: MSI vector pool (arch half #ifdef'd out under NANOS_HOST_TEST)
TEST_MODULES+= drivers/Framebuffer.cpp drivers/Font8x16.cpp drivers/FbConsole.cpp drivers/Fbdev.cpp drivers/KeyboardDevice.cpp drivers/Pty.cpp
TEST_MODULES+= drivers/DrmDevice.cpp   # /dev/dri forwarder — pure ioctl/mmap-offset dispatch (test_drm_node.cpp)
TEST_MODULES+= kernel/vt/VtConsole.cpp kernel/vt/VtManager.cpp drivers/VtTty.cpp   # virtual terminals: per-VT console + switching manager + /dev/ttyN
TEST_MODULES+= kext/mouse/MouseDevice.cpp   # MI half of the mouse kext (PS/2 decode -> evdev)
# nanowm (window server) pure cores — userland C, host-tested as C++ (g++ treats .c as C++).
# vtfont.c supplies the shared nx_font8x16 the gfx rasterizer draws with.
TEST_MODULES+= user/libnw/nwproto.c user/libnw/nw_gfx.c user/term/vtfont.c
TEST_MODULES+= user/libnw/nwfont.c user/libnw/stb_impl.c   # TTF font engine (3rd-party stb; not gated)
TEST_MODULES+= user/libnw/nw_settings.c   # pure desktop-preferences model (parse/serialize/derive)
TEST_MODULES+= user/nwm/nwm_core.c user/nwm/nw_compose.c
TEST_MODULES+= user/nwm/nw_backdrop.c   # pure backdrop-blur math (rect helpers, downsample, upsample)
TEST_MODULES+= user/libnwui/nwui_core.c   # the pure UI-toolkit core (tree/layout/events)
TEST_MODULES+= user/libnwui/nwui_png.c    # the PNG decoder + nwui_image_load_png file loader (not gated: many format branches exercised live)
TEST_MODULES+= user/libnwui/nwui_fs.c     # reusable directory-enumeration shim (opendir/readdir wrapper)
TEST_MODULES+= user/term/vt.c             # the pure VT/ANSI terminal engine (shared by nterm/terminal)
TEST_MODULES+= kernel/Pci.cpp             # MI PCI enumeration/BAR decode (mock config-space backend)
TEST_MODULES+= kernel/MsiRouter.cpp       # MI MSI/MSI-X cap walk + programming (mock config space)
TEST_MODULES+= kext/e1000/e1000_core.cpp  # pure helpers (ring/desc encode); engine half #ifdef'd out
TEST_MODULES+= kext/i219/i219_phy.cpp     # pure MDIC command encoder; ich9lan engine half #ifdef'd out
TEST_MODULES+= net/Net.cpp net/NetBuf.cpp net/NetDevice.cpp net/Loopback.cpp   # MI net core
TEST_MODULES+= net/Ether.cpp net/Arp.cpp net/Ip.cpp net/Route.cpp net/Icmp.cpp   # L2 + ARP + IPv4 + ICMP
TEST_MODULES+= net/Socket.cpp net/Udp.cpp net/Raw.cpp net/Tcp.cpp net/Packet.cpp net/Unix.cpp   # socket layer + UDP + RAW + TCP + AF_PACKET + AF_UNIX
TEST_MODULES+= net/NetProc.cpp net/NetStats.cpp   # /proc/net renderers + SNMP counters (FAZA 14)
TEST_MODULES+= user/libc-glue/resolv_parse.c   # pure resolver parsers (resolv.conf/services/protocols/DNS, FAZA E)
TEST_MODULES+= user/libc-glue/crypt.c          # crypt(3) SHA-512 ($6$) — known-answer vs openssl
TEST_MODULES+= usb/UsbCore.cpp                  # MI USB core: descriptors + enumeration (mock HC)
TEST_MODULES+= usb/UsbHid.cpp                   # MI USB-HID boot keyboard/mouse decoders
TEST_MODULES+= usb/UsbMsc.cpp                   # MI USB mass-storage: BOT + SCSI read
TEST_MODULES+= drivers/UsbMscBlockDevice.cpp    # MI BlockDevice adapter over USB MSC
TEST_MODULES+= kernel/PartitionTable.cpp        # MI MBR+GPT root-partition discovery
# LinuxKPI shim — pure primitives (compiled as C++ by g++; kept C-valid for the kext).
# Not in COV_PATTERNS (not gated), just compiled + exercised by tests/test_linuxkpi_*.
TEST_MODULES+= linuxkpi/kpi_slab.c linuxkpi/kpi_print.c linuxkpi/kpi_idr.c linuxkpi/kpi_sort.c linuxkpi/kpi_time.c linuxkpi/kpi_irq.c linuxkpi/kpi_kthread.c linuxkpi/kpi_firmware.c linuxkpi/kpi_shrinker.c linuxkpi/kpi_rcu.c
# lcov patterns selecting the modules whose coverage is gated (String is support).
COV_PATTERNS="*/RamBlockDevice.*" "*/DeviceManager.*" "*/Vfs.*" "*/ExtFilesystem.*" "*/Ext2Filesystem.*" "*/Ext4Filesystem.*" "*/ExtAllocator.*" "*/BlockCache.*" "*/ExtCsum.*" "*/Crc32c.*" "*/Journal.*" "*/SynthFs.*" "*/RamFs.*" "*/Syscall.*" "*/Cred.*" "*/NxeLoader.*" "*/KeyDecoder.*" "*/Process.*" "*/Signal.*" "*/Futex.*" "*/Csprng.*" "*/Acpi.*" "*/Framebuffer.*" "*/FbConsole.*" "*/Fbdev.*" "*/KeyboardDevice.*" "*/Pty.*" "*/MouseDevice.*" "*/MultibootMmap.*" "*/FrameAllocator.*" "*/Heap.*" "*/AddressSpace.*" "*/nwproto.*" "*/nw_gfx.*" "*/nw_settings.*" "*/nwm_core.*" "*/nw_compose.*" "*/nw_backdrop.*" "*/nwui_core.*" "*/vt.*" "*/Pci.*" "*/MsiRouter.*" "*/lapic_x86_64.*" "*/e1000_core.*" "*/i219_phy.*" "*/Net.*" "*/NetBuf.*" "*/NetDevice.*" "*/Loopback.*" "*/Ether.*" "*/Arp.*" "*/Ip.*" "*/Route.*" "*/Icmp.*" "*/Socket.*" "*/Udp.*" "*/Raw.*" "*/Tcp.*" "*/Packet.*" "*/Unix.*" "*/NetProc.*" "*/NetStats.*" "*/resolv_parse.*" "*/crypt.*" "*/UsbCore.*" "*/UsbHid.*" "*/UsbMsc.*" "*/UsbMscBlockDevice.*" "*/PartitionTable.*"
COV_INFO=/tmp/cov.info
COV_MIN=90
# The repo is bind-mounted from a case-insensitive macOS FS, which makes
# `#include <string.h>` resolve to String.h (infinite recursion). Compile from a
# copy on the container's own case-sensitive FS so <string.h> means libc.
BUILDDIR=/tmp/nbuild

_test:
	@rm -rf $(BUILDDIR) && mkdir -p $(BUILDDIR) && \
	 tar -cf - --exclude=.git --exclude=disk --exclude=bin --exclude=iso --exclude=coverage -C /src . | tar -xf - -C $(BUILDDIR)
	cd $(BUILDDIR) && $(HOST_CXX) $(HOST_CXXFLAGS) -c $(TEST_SRCS) $(TEST_MODULES)
	cd $(BUILDDIR) && $(HOST_CXX) $(HOST_CXXFLAGS) -o $(TEST_BIN) *.o
	cd $(BUILDDIR) && $(TEST_BIN)
	@cd $(BUILDDIR) && lcov --capture --directory . --output-file $(COV_INFO) --no-external --ignore-errors mismatch,unused,empty >/dev/null 2>&1
	@lcov --extract $(COV_INFO) $(COV_PATTERNS) --output-file $(COV_INFO).f --ignore-errors unused,empty >/dev/null 2>&1
	@lcov --list $(COV_INFO).f 2>/dev/null || true
	@pct=$$(lcov --summary $(COV_INFO).f 2>&1 | grep -oP 'lines[.]*: \K[0-9.]+'); \
	 echo "==> Line coverage (modules under test): $${pct:-0}%"; \
	 awk "BEGIN{exit !($${pct:-0}+0 >= $(COV_MIN))}" \
	   || { echo "FAIL: line coverage below $(COV_MIN)%"; exit 1; }

_coverage: _test
	@genhtml $(COV_INFO).f --output-directory /src/coverage >/dev/null 2>&1 && \
	 echo "==> HTML coverage report: coverage/index.html"

endif

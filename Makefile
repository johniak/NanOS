# Architecture selection: machine-dependent knobs come from arch/$(ARCH)/arch.mk
# (CROSS, ARCH_VPATH, ARCH_INCLUDES, ARCH_LINKER, ARCH_SOURCES).
ARCH ?= x86
include arch/$(ARCH)/arch.mk

# Machine-independent objects (portable across architectures).
MI_SOURCES=kmain.o Kernel.o Console.o ExtFilesystem.o SynthFs.o RamFs.o RamBlockDevice.o DeviceManager.o Vfs.o
MI_SOURCES+= Crc32c.o BlockCache.o ExtCsum.o ExtAllocator.o Journal.o
MI_SOURCES+= Framebuffer.o Font8x16.o FbConsole.o vtk.o Fbdev.o Fb0Device.o DrmDevice.o KeyboardDevice.o Pty.o
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
IMAGE_GRUB2=disk/image-grub2.img
# Partition starts at LBA 2048 (1MiB offset)
IMAGE_GRUB2_PART=$(IMAGE_GRUB2)?offset=1048576
# x86_64 staged GRUB disk image (Plan 5): a separate image carrying the staged ELF64 kernel
# + an ext4 partition for /disks/main. Same partition layout/offset as the i686 image.
IMAGE64_GRUB2=disk/image64-grub2.img
# Hybrid GPT+Limine layout: the ext4 root (P3) starts at 34 MiB (after bios_boot @1MiB + ESP @2MiB,32MiB).
IMAGE64_GRUB2_PART=$(IMAGE64_GRUB2)?offset=69206016

DOCKER_IMAGE=nanos-build
# Build the image for the host's NATIVE architecture (no --platform): the i686-elf cross
# toolchain and all image tools are arch-agnostic, so on Apple Silicon this runs natively
# instead of under QEMU amd64 emulation — the single biggest build speedup. The output
# (i686 kernel + i386-pc GRUB image) is identical regardless of build-host arch.
DOCKER_RUN=docker run --rm -v $(CURDIR):/src -w /src $(DOCKER_IMAGE)

ifeq ($(wildcard /etc/nanos-build),)
# ============================================================================
# HOST side (macOS): build inside Docker, run QEMU natively.
# ============================================================================

docker-image:
	docker build -t $(DOCKER_IMAGE) docker/

build: docker-image
	$(DOCKER_RUN) sh -c 'make -j"$$(nproc)" _all'

image: docker-image
	$(DOCKER_RUN) sh -c 'make -j"$$(nproc)" _image'

iso: docker-image
	$(DOCKER_RUN) sh -c 'make -j"$$(nproc)" _iso'

# GNU bash (optional, external). The bash sources AND its build scaffolding (nx-gcc wrapper,
# build.sh, config.cache, port patches) live in a SEPARATE fork repo — NOT here. This target
# only runs that build inside the cross-toolchain container (both repos bind-mounted) and
# copies the resulting bash.nxe into bin/, where _image installs it as the /apps/bash bundle.
# Absent fork => the target errors clearly; `make image` itself never depends on this, so a
# missing fork can't break a normal build.
BASH_FORK ?= $(HOME)/Projects/bash-nanos
# Arch-aware (mirrors smoke64 / the in-tree x86_64 userland): for ARCH=x86_64 the fork builds
# against the 64-bit picolibc sysroot + x86_64-elf-gcc, links the elf64 .nxe linker script and
# turns the ELF into a v4 .nxe with mknx64 (NX_LP64=1 fixes bash's LP64 sizeof answers). i686
# passes no env, so build.sh/nx-gcc fall back to their 32-bit defaults — unchanged.
ifeq ($(ARCH),x86_64)
BASH_ENV = -e NX_CC=x86_64-elf-gcc -e NX_HOST=x86_64-elf -e NX_PICO=/opt/picolibc/x86_64-elf \
  -e NX_MKNX=/src/bin/mknx64 -e NX_LDSCRIPT=/src/arch/x86_64/user-nx.ld \
  -e 'NX_ARCHFLAGS=-mcmodel=small -mno-red-zone' -e NX_LP64=1
# The fork links /src/bin/{crt0.o,nxhdr.o,libc.ndl.a} + mknx64 — they must be the ELF64 build.
BASH_PREREQ = $(DOCKER_RUN) sh -c 'make ARCH=x86_64 bin/libc.ndl bin/libc.ndl.a bin/crt0.o bin/nxhdr.o bin/mknx64'
else
BASH_ENV =
BASH_PREREQ = @true
endif
bash: docker-image
	@test -f "$(BASH_FORK)/nanos/build.sh" || { echo "bash fork not found at $(BASH_FORK)/nanos (set BASH_FORK=/path/to/bash-nanos)"; exit 1; }
	$(BASH_PREREQ)
	docker run --rm -v $(CURDIR):/src -v "$(BASH_FORK)":/bash $(BASH_ENV) -w /bash $(DOCKER_IMAGE) sh /bash/nanos/build.sh build
	cp "$(BASH_FORK)/nanos/bash.nxe" $(BINFOLDER)bash.nxe
	@echo "staged $(BINFOLDER)bash.nxe — run 'make image' (i686) or 'make image64' (x86_64) to install it"

# GNU grep / vim / bzip2 (optional, external). The upstream sources live in the nanos-sdk work
# dir. These targets are ARCH-AWARE:
#   * i686 (default): copy the hand-built .nxe the nanos-sdk produced (the original 32-bit flow,
#     left untouched — `make image` never depends on it, so a missing artifact can't break a build).
#   * x86_64: rebuild the .nxe from source inside the nanos-build container via the reproducible
#     driver scripts/nx-port-build.sh (nx-gcc wrapper -> picolibc + libc-glue + a NanOS .nxe link,
#     mknx64 -> v4 .nxe). Mirrors `make bash`: the same NX_* env selects the 64-bit toolchain,
#     linker script, mknx and LP64 sizes; the driver applies the per-app LP64/POSIX fixes.
SDK_WORK ?= $(HOME)/Projects/nanos-sdk-work
# Shared x86_64 build env + the ELF64 startup/libc prereqs (same set `make bash` rebuilds).
NXPORT_ENV = -e NX_CC=x86_64-elf-gcc -e NX_HOST=x86_64-elf -e NX_PICO=/opt/picolibc/x86_64-elf \
  -e NX_MKNX=/src/bin/mknx64 -e NX_LDSCRIPT=/src/arch/x86_64/user-nx.ld \
  -e 'NX_ARCHFLAGS=-mcmodel=small -mno-red-zone' -e NX_LP64=1
NXPORT_PREREQ = $(DOCKER_RUN) sh -c 'make ARCH=x86_64 bin/libc.ndl bin/libc.ndl.a bin/crt0.o bin/nxhdr.o bin/mknx64'
# docker run that mounts BOTH this checkout (/src) and the app source ($1 -> /work/src).
NXPORT_RUN = docker run --rm -v $(CURDIR):/src $(NXPORT_ENV) -w /src

grep:
ifeq ($(ARCH),x86_64)
	$(NXPORT_PREREQ)
	$(NXPORT_RUN) -v "$(SDK_WORK)/grep-3.11":/work/src $(DOCKER_IMAGE) sh /src/scripts/nx-port-build.sh grep
	cp "$(SDK_WORK)/grep-3.11/grep.nxe" $(BINFOLDER)grep.nxe
else
	@test -f "$(SDK_WORK)/grep-3.11/src/grep.nxe" || { echo "grep.nxe not found at $(SDK_WORK)/grep-3.11/src (build it with the nanos-sdk first)"; exit 1; }
	cp "$(SDK_WORK)/grep-3.11/src/grep.nxe" $(BINFOLDER)grep.nxe
endif
	@echo "staged $(BINFOLDER)grep.nxe — run 'make image' (i686) or 'make image64' (x86_64) to install it into /nanos/bin"

# toybox (0BSD multicall): the user-identity tools (login/su/passwd/id/groups/whoami). x86_64 only
# (i686 is frozen). Built via the nanos-sdk port flow; installed as a single setuid-root toybox.nxe
# with a per-command symlink farm (image64 install). Source: $(SDK_WORK)/toybox-0.8.11.
.PHONY: toybox
toybox:
ifeq ($(ARCH),x86_64)
	$(NXPORT_PREREQ)
	$(NXPORT_RUN) -v "$(SDK_WORK)/toybox-0.8.11":/work/src $(DOCKER_IMAGE) sh /src/scripts/nx-port-build.sh toybox
	cp "$(SDK_WORK)/toybox-0.8.11/toybox.nxe" $(BINFOLDER)toybox.nxe
else
	@echo "toybox is x86_64-only (i686 is frozen)"; exit 1
endif
	@echo "staged $(BINFOLDER)toybox.nxe — run 'make image64' to install it + the login/su/passwd/id symlinks"

# sudo (Todd Miller, ISC license): real sudo with the sudoers policy linked statically. x86_64
# only. Installed setuid-root in /nanos/bin; /etc/sudoers (%wheel) is seeded already. Source:
# $(SDK_WORK)/sudo-1.9.15p5.
.PHONY: sudo
sudo:
ifeq ($(ARCH),x86_64)
	$(NXPORT_PREREQ)
	$(NXPORT_RUN) -v "$(SDK_WORK)/sudo-1.9.15p5":/work/src $(DOCKER_IMAGE) sh /src/scripts/nx-port-build.sh sudo
	cp "$(SDK_WORK)/sudo-1.9.15p5/sudo.nxe" $(BINFOLDER)sudo.nxe
else
	@echo "sudo is x86_64-only (i686 is frozen)"; exit 1
endif
	@echo "staged $(BINFOLDER)sudo.nxe — run 'make image64' to install it setuid-root into /nanos/bin"

# Vim (the editor). ARCH-AWARE:
#   * i686 (default): copy the hand-built 32-bit .nxe the nanos-sdk produced (original flow, intact).
#   * x86_64: build from source via the REPRODUCIBLE nanos-port driver (mirrors `make ping`/`make
#     ncurses`) against the x86_64-nanos sysroot, linking libtinfo.a from the ncurses port. The
#     manifest ($(VIM_PORT)/nxport.toml) REUSES the i686 vim_cv_* cross-cache; hooks/pre_configure.sh
#     scrubs the stale i686 build state the `dir:` copy carries over. We first refresh the SDK sysroot
#     from THIS checkout (POSIX headers + libc.ndl{,.a} + the nx-dllimport.h data-import shim + the
#     crt0/nxhdr startup objects + mknx64) so the x86_64 build tracks the live ABI. Requires `make
#     ARCH=x86_64 ncurses` first (libtinfo.a + curses.h/term.h in the sysroot). `make image*` never
#     depends on this; a missing artifact is skipped by _image/_image64.
VIM_PORT := $(SDK_WORK)/vim-port
VIM_SRC  := $(SDK_WORK)/vim
ifeq ($(ARCH),x86_64)
VIM_TRIPLE   := x86_64-nanos
# CFLAGS = NanOS x86_64 user ABI (fixed-base ET_EXEC, small model, no red zone) + the dllimport shim
# force-included into every TU (x86_64 references picolibc's stdout/stderr/errno DATA RIP-relative,
# which mknx routes through __imp_<name> IAT slots). LDFLAGS=-no-pie keeps the fixed-base layout.
VIM_PORT_ENV  = -e NX_HOST=x86_64-nanos -e NX_LP64=1 \
  -e CFLAGS="-O2 -fno-pie -mcmodel=small -mno-red-zone -include nx-dllimport.h" \
  -e LDFLAGS="-no-pie"
endif
vim:
ifeq ($(ARCH),x86_64)
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
else
	@test -f "$(SDK_WORK)/vim/src/vim.nxe" || { echo "vim.nxe not found at $(SDK_WORK)/vim/src (build it with the nanos-sdk first)"; exit 1; }
	cp "$(SDK_WORK)/vim/src/vim.nxe" $(BINFOLDER)vim.nxe
endif
	@echo "staged $(BINFOLDER)vim.nxe — run 'make image' (i686) or 'make image64' (x86_64) to install it into /apps/vim"

# htop (interactive process monitor). x86_64-ONLY (like the vim port): builds from source via the
# reproducible nanos-port driver against the x86_64-nanos sysroot, linking libncurses.a/libtinfo.a
# from the ncurses port. hooks/pre_configure.sh maps *nanos* to htop's Linux /proc backend, which
# reads NanOS's Linux-style /proc (SynthFs). Requires `make ARCH=x86_64 ncurses` first. `make
# image*` never depends on this; a missing artifact is skipped by _image64.
HTOP_PORT := $(SDK_WORK)/htop-port
HTOP_TRIPLE := x86_64-nanos
htop:
ifeq ($(ARCH),x86_64)
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
else
	@echo "htop port is x86_64-only — run 'make ARCH=x86_64 htop'"; exit 1
endif
	@echo "staged $(BINFOLDER)htop.nxe — run 'make image64' to install it into /nanos/bin"

# SQLite 3.46.1 — the real `sqlite3` command-line shell + libsqlite.ndl shared library. x86_64-ONLY.
# Built from the SQLite fork (amalgamation sqlite3.c + the CLI shell.c) via the reproducible
# nx-port-build.sh driver: SQLite has no configure step, so the driver just cross-compiles + links
# the .nxe. nanos/compat.c supplies the advisory-lock no-op fcntl (NanOS has no POSIX byte-range
# locks); fsync/ftruncate are REAL, so a database on the read-write ext4 /disks/main is durable and
# crash-consistent. Produces bin/sqlite3.nxe (-> /nanos/bin) and bin/libsqlite.ndl{,.a} (-> /nanos/lib,
# for apps that want to import the SQL engine by name). `make image*` never depends on this.
SQLITE_FORK ?= $(HOME)/Projects/sqlite-nanos
sqlite:
ifeq ($(ARCH),x86_64)
	@test -f "$(SQLITE_FORK)/sqlite3.c" || { echo "sqlite fork not found at $(SQLITE_FORK) (set SQLITE_FORK=/path/to/sqlite-nanos)"; exit 1; }
	$(NXPORT_PREREQ)
	$(NXPORT_RUN) -v "$(SQLITE_FORK)":/work/src $(DOCKER_IMAGE) sh /src/scripts/nx-port-build.sh sqlite3
	cp "$(SQLITE_FORK)/sqlite3.nxe" $(BINFOLDER)sqlite3.nxe
	@cp "$(SQLITE_FORK)/libsqlite.ndl"   $(BINFOLDER)libsqlite.ndl   2>/dev/null || true
	@cp "$(SQLITE_FORK)/libsqlite.ndl.a" $(BINFOLDER)libsqlite.ndl.a 2>/dev/null || true
else
	@echo "sqlite port is x86_64-only — run 'make ARCH=x86_64 sqlite'"; exit 1
endif
	@echo "staged $(BINFOLDER)sqlite3.nxe (+ libsqlite.ndl) — run 'make image64' to install into /nanos/bin (+ /nanos/lib)"

bzip2:
ifeq ($(ARCH),x86_64)
	$(NXPORT_PREREQ)
	$(NXPORT_RUN) -v "$(SDK_WORK)/bzip2-1.0.8":/work/src $(DOCKER_IMAGE) sh /src/scripts/nx-port-build.sh bzip2
	cp "$(SDK_WORK)/bzip2-1.0.8/bzip2.nxe" $(BINFOLDER)bzip2.nxe
else
	@test -f "$(SDK_WORK)/bzip2-1.0.8/bzip2.nxe" || { echo "bzip2.nxe not found at $(SDK_WORK)/bzip2-1.0.8 (build it with the nanos-sdk first)"; exit 1; }
	cp "$(SDK_WORK)/bzip2-1.0.8/bzip2.nxe" $(BINFOLDER)bzip2.nxe
endif
	@echo "staged $(BINFOLDER)bzip2.nxe — run 'make image' (i686) or 'make image64' (x86_64) to install it into /nanos/bin"

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
# crt0/nxhdr the toolchain default-links are refreshed too (like `make git`). i686 is unchanged.
NANOS_SDK   ?= $(HOME)/Projects/nanos-sdk
SDK_TC      := $(SDK_WORK)/toolchain
PING_PORT   := $(SDK_WORK)/inetutils-port
ifeq ($(ARCH),x86_64)
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
else
PING_TRIPLE  := i686-nanos
PING_PORT_ENV =
PING_PREREQ   = @true
PING_STARTUP  = true
PING_PRECMD   =
endif
ping: bin/libc.ndl bin/libc.ndl.a
	@test -d "$(SDK_TC)/$(PING_TRIPLE)/include" || { echo "nanos-sdk $(PING_TRIPLE) toolchain not found at $(SDK_TC)"; exit 1; }
	@test -f "$(PING_PORT)/nxport.toml"     || { echo "inetutils port not found at $(PING_PORT)/nxport.toml"; exit 1; }
	# x86_64: (re)build the in-tree 64-bit libc/crt0/nxhdr/mknx first; i686 uses the prereqs as-is.
	$(PING_PREREQ)
	# Refresh the SDK sysroot from this repo (the source of truth for headers + libc).
	cp -R user/libc-glue/include/. "$(SDK_TC)/$(PING_TRIPLE)/include/"
	cp kernel/SyscallNr.h          "$(SDK_TC)/$(PING_TRIPLE)/include/SyscallNr.h"
	# The dllimport shim (stdin/stdout/stderr/environ/_ctype_b -> libc.ndl IAT slots). x86_64
	# code references these DATA exports RIP-relative (R_X86_64_PC32), which mknx can't auto-
	# import; the shim turns each into an __imp_<name> slot deref. The port's post_configure
	# hook #includes it into config.h (only for x86_64; i686 uses absolute relocs and skips it).
	cp user/libc-glue/nx-dllimport.h "$(SDK_TC)/$(PING_TRIPLE)/include/nx-dllimport.h"
	cp $(BINFOLDER)libc.ndl.a      "$(SDK_TC)/$(PING_TRIPLE)/lib/libc.a"
	cp $(BINFOLDER)libc.ndl        "$(SDK_TC)/$(PING_TRIPLE)/lib/libc.ndl"
	$(PING_STARTUP)
	docker run --rm \
	  -v "$(SDK_TC)":/work/toolchain -v "$(PING_PORT)":/work/port -v "$(NANOS_SDK)":/sdk \
	  -e SDK=/sdk $(PING_PORT_ENV) -e PATH="/work/toolchain/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
	  -w /work/port nanos-sdk-dev:latest sh -c '$(PING_PRECMD) python3 /sdk/port/nanos-port /work/port'
	cp "$(PING_PORT)/ping.nxe" $(BINFOLDER)ping.nxe
	@echo "staged $(BINFOLDER)ping.nxe — run 'make image' (i686) or 'make image64' (x86_64) to install it into /nanos/bin"

# GNU wget 1.21.4 (optional, external). Unmodified upstream, HTTP-only (no TLS yet). Same
# reproducible flow as `make ping`: refresh the SDK sysroot from this checkout, drive nanos-port.
# Manifest at $(SDK_WORK)/wget-port. `make image` never depends on this.
WGET_PORT   := $(SDK_WORK)/wget-port
wget: bin/libc.ndl bin/libc.ndl.a
	@test -d "$(SDK_TC)/i686-nanos/include" || { echo "nanos-sdk toolchain not found at $(SDK_TC)"; exit 1; }
	@test -f "$(WGET_PORT)/nxport.toml"     || { echo "wget port not found at $(WGET_PORT)/nxport.toml"; exit 1; }
	cp -R user/libc-glue/include/. "$(SDK_TC)/i686-nanos/include/"
	cp kernel/SyscallNr.h          "$(SDK_TC)/i686-nanos/include/SyscallNr.h"
	cp $(BINFOLDER)libc.ndl.a      "$(SDK_TC)/i686-nanos/lib/libc.a"
	cp $(BINFOLDER)libc.ndl        "$(SDK_TC)/i686-nanos/lib/libc.ndl"
	docker run --rm \
	  -v "$(SDK_TC)":/work/toolchain -v "$(WGET_PORT)":/work/port -v "$(NANOS_SDK)":/sdk \
	  -e SDK=/sdk -e PATH="/work/toolchain/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
	  -w /work/port nanos-sdk-dev:latest python3 /sdk/port/nanos-port /work/port
	cp "$(WGET_PORT)/wget.nxe" $(BINFOLDER)wget.nxe
	@echo "staged $(BINFOLDER)wget.nxe — run 'make image' to install it into /nanos/bin"

# GNU git 2.54.0 (optional, external). Unmodified upstream source built for NanOS via nanos-port's
# build="make" path (git ships its own Makefile; the cross knobs live in a committed config.mak in
# the source tree — see $(GIT_PORT)/git-2.54.0/config.mak). Local-only git (no http/curl/openssl);
# THREADS ON (pack-objects delta search) and run-command's fork both exercised. The single git.nxe
# bundles all builtins (SKIP_DASHED_BUILT_INS). Same reproducible flow as ping/wget. `make image`
# never depends on this. Requires `make build` first (refreshes the pthread-enabled libc.ndl).
GIT_PORT    := $(SDK_WORK)/git-port
git: bin/libc.ndl bin/libc.ndl.a
	@test -d "$(SDK_TC)/i686-nanos/include" || { echo "nanos-sdk toolchain not found at $(SDK_TC)"; exit 1; }
	@test -f "$(GIT_PORT)/nxport.toml"      || { echo "git port not found at $(GIT_PORT)/nxport.toml"; exit 1; }
	cp -R user/libc-glue/include/. "$(SDK_TC)/i686-nanos/include/"
	cp kernel/SyscallNr.h          "$(SDK_TC)/i686-nanos/include/SyscallNr.h"
	cp $(BINFOLDER)libc.ndl.a      "$(SDK_TC)/i686-nanos/lib/libc.a"
	cp $(BINFOLDER)libc.ndl        "$(SDK_TC)/i686-nanos/lib/libc.ndl"
	# Refresh the startup objects too: crt0.o carries the main-thread TLS bootstrap
	# (__nx_init_tls -> set_thread_area), so a stale crt0 leaves %gs:0 unset and the first
	# errno access faults (cr2=0x1c). The sysroot copy must track this checkout's crt0/nxhdr.
	cp $(BINFOLDER)crt0.o          "$(SDK_TC)/i686-nanos/lib/crt0.o"
	cp $(BINFOLDER)nxhdr.o         "$(SDK_TC)/i686-nanos/lib/nxhdr.o"
	docker run --rm \
	  -v "$(SDK_TC)":/work/toolchain -v "$(GIT_PORT)":/work/port -v "$(NANOS_SDK)":/sdk \
	  -e SDK=/sdk -e PATH="/work/toolchain/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
	  -w /work/port nanos-sdk-dev:latest python3 /sdk/port/nanos-port /work/port
	cp "$(GIT_PORT)/git.nxe" $(BINFOLDER)git.nxe
	@echo "staged $(BINFOLDER)git.nxe — run 'make image' to install it into /nanos/bin (+ /nanos/libexec/git-core)"

# GNU inetutils SERVICES build (FAZA H): the internet super-server inetd (and, as the build
# grows, telnetd + the telnet/ifconfig/traceroute clients) from the SAME inetutils source as
# `make ping`, but configured with servers enabled. Manifest at $(SERVICES_PORT)/nxport.toml;
# hooks/post_build.sh mknx's the extra binaries. Same reproducible flow as ping/wget.
SERVICES_PORT := $(SDK_WORK)/inetutils-services-port
inetd: bin/libc.ndl bin/libc.ndl.a
	@test -d "$(SDK_TC)/i686-nanos/include" || { echo "nanos-sdk toolchain not found at $(SDK_TC)"; exit 1; }
	@test -f "$(SERVICES_PORT)/nxport.toml"  || { echo "inetutils services port not found at $(SERVICES_PORT)/nxport.toml"; exit 1; }
	cp -R user/libc-glue/include/. "$(SDK_TC)/i686-nanos/include/"
	cp kernel/SyscallNr.h          "$(SDK_TC)/i686-nanos/include/SyscallNr.h"
	cp $(BINFOLDER)libc.ndl.a      "$(SDK_TC)/i686-nanos/lib/libc.a"
	cp $(BINFOLDER)libc.ndl        "$(SDK_TC)/i686-nanos/lib/libc.ndl"
	docker run --rm \
	  -v "$(SDK_TC)":/work/toolchain -v "$(SERVICES_PORT)":/work/port -v "$(NANOS_SDK)":/sdk \
	  -e SDK=/sdk -e PATH="/work/toolchain/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
	  -w /work/port nanos-sdk-dev:latest python3 /sdk/port/nanos-port /work/port
	cp "$(SERVICES_PORT)/inetd.nxe" $(BINFOLDER)inetd.nxe
	@for b in telnetd telnet ifconfig traceroute; do \
	  test -f "$(SERVICES_PORT)/$$b.nxe" && cp "$(SERVICES_PORT)/$$b.nxe" $(BINFOLDER)$$b.nxe && echo "  staged $$b.nxe" || true; \
	done
	@echo "staged $(BINFOLDER)inetd.nxe (+ telnetd/telnet/ifconfig/traceroute) — run 'make image' to install"

# darkhttpd (optional, external): single-file HTTP/1.1 static server (FAZA H4). Built by the
# nanos-sdk from $(HTTPD_PORT)/nxport.toml (build=make, -DNO_IPV6). Same reproducible flow.
HTTPD_PORT := $(SDK_WORK)/darkhttpd-port
httpd: bin/libc.ndl bin/libc.ndl.a
	@test -d "$(SDK_TC)/i686-nanos/include" || { echo "nanos-sdk toolchain not found at $(SDK_TC)"; exit 1; }
	@test -f "$(HTTPD_PORT)/nxport.toml"     || { echo "darkhttpd port not found at $(HTTPD_PORT)/nxport.toml"; exit 1; }
	cp -R user/libc-glue/include/. "$(SDK_TC)/i686-nanos/include/"
	cp kernel/SyscallNr.h          "$(SDK_TC)/i686-nanos/include/SyscallNr.h"
	cp $(BINFOLDER)libc.ndl.a      "$(SDK_TC)/i686-nanos/lib/libc.a"
	cp $(BINFOLDER)libc.ndl        "$(SDK_TC)/i686-nanos/lib/libc.ndl"
	docker run --rm \
	  -v "$(SDK_TC)":/work/toolchain -v "$(HTTPD_PORT)":/work/port -v "$(NANOS_SDK)":/sdk \
	  -e SDK=/sdk -e PATH="/work/toolchain/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
	  -w /work/port nanos-sdk-dev:latest python3 /sdk/port/nanos-port /work/port
	cp "$(HTTPD_PORT)/darkhttpd.nxe" $(BINFOLDER)darkhttpd.nxe
	@echo "staged $(BINFOLDER)darkhttpd.nxe — run 'make image' to install it into /nanos/bin"

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
# i686 is unchanged (generic `gcc` target, absolute relocs, no shim).
# ---- libdrm 2.4.123 (virtgpu-only, static) — the GL stack's DRM userspace (Task 7) ----
# x86_64 only. nanos-port's meson build type hardcodes the i686 nanos-cross.meson, so this port
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

OPENSSL_PORT := $(SDK_WORK)/openssl-port
ifeq ($(ARCH),x86_64)
OPENSSL_TRIPLE  := x86_64-nanos
OPENSSL_PORT_ENV = -e NX_HOST=x86_64-nanos -e NX_LP64=1
OPENSSL_PREREQ   = $(NXPORT_PREREQ)
OPENSSL_STARTUP  = cp $(BINFOLDER)crt0.o "$(SDK_TC)/$(OPENSSL_TRIPLE)/lib/crt0.o"; cp $(BINFOLDER)nxhdr.o "$(SDK_TC)/$(OPENSSL_TRIPLE)/lib/nxhdr.o"; cp $(BINFOLDER)mknx64 "$(SDK_TC)/bin/$(OPENSSL_TRIPLE)-mknx"
else
OPENSSL_TRIPLE  := i686-nanos
OPENSSL_PORT_ENV =
OPENSSL_PREREQ   = @true
OPENSSL_STARTUP  = true
endif
openssl: bin/libc.ndl bin/libc.ndl.a
	@test -d "$(SDK_TC)/$(OPENSSL_TRIPLE)/include" || { echo "nanos-sdk $(OPENSSL_TRIPLE) toolchain not found at $(SDK_TC)"; exit 1; }
	@test -f "$(OPENSSL_PORT)/nxport.toml"   || { echo "openssl port not found at $(OPENSSL_PORT)/nxport.toml"; exit 1; }
	# x86_64: (re)build the in-tree 64-bit libc/crt0/nxhdr/mknx first; i686 uses the prereqs as-is.
	$(OPENSSL_PREREQ)
	cp -R user/libc-glue/include/. "$(SDK_TC)/$(OPENSSL_TRIPLE)/include/"
	cp kernel/SyscallNr.h          "$(SDK_TC)/$(OPENSSL_TRIPLE)/include/SyscallNr.h"
	# The dllimport shim (stdin/stdout/stderr/environ/_ctype_b -> libc.ndl IAT slots). x86_64
	# code references these DATA exports RIP-relative (R_X86_64_PC32), which mknx can't auto-import;
	# pre_configure -include's it into every TU via CFLAGS (OpenSSL's make build has no autotools
	# config.h to append to). i686 uses absolute relocs and skips it (NX_HOST != x86_64-nanos).
	cp user/libc-glue/nx-dllimport.h "$(SDK_TC)/$(OPENSSL_TRIPLE)/include/nx-dllimport.h"
	cp $(BINFOLDER)libc.ndl.a      "$(SDK_TC)/$(OPENSSL_TRIPLE)/lib/libc.a"
	cp $(BINFOLDER)libc.ndl        "$(SDK_TC)/$(OPENSSL_TRIPLE)/lib/libc.ndl"
	$(OPENSSL_STARTUP)
	docker run --rm \
	  -v "$(SDK_TC)":/work/toolchain -v "$(OPENSSL_PORT)":/work/port -v "$(NANOS_SDK)":/sdk \
	  -e SDK=/sdk $(OPENSSL_PORT_ENV) -e PATH="/work/toolchain/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
	  -w /work/port nanos-sdk-dev:latest python3 /sdk/port/nanos-port /work/port
	cp "$(OPENSSL_PORT)/openssl.nxe" $(BINFOLDER)openssl.nxe
	@echo "staged $(BINFOLDER)openssl.nxe — run 'make image' (i686) or 'make image64' (x86_64) to install it into /nanos/bin"

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
# i686 keeps the original 32-bit flags (generic gcc, absolute relocs, no shim).
DROPBEAR_PORT := $(SDK_WORK)/dropbear-port
ifeq ($(ARCH),x86_64)
DROPBEAR_TRIPLE   := x86_64-nanos
DROPBEAR_PORT_ENV  = -e NX_HOST=x86_64-nanos -e NX_LP64=1 \
  -e 'CFLAGS=-Os -fno-pie -fno-PIC -mcmodel=small -mno-red-zone -include nx-dllimport.h' \
  -e 'LDFLAGS=-no-pie'
DROPBEAR_PREREQ    = $(NXPORT_PREREQ)
DROPBEAR_STARTUP   = cp $(BINFOLDER)crt0.o "$(SDK_TC)/$(DROPBEAR_TRIPLE)/lib/crt0.o"; cp $(BINFOLDER)nxhdr.o "$(SDK_TC)/$(DROPBEAR_TRIPLE)/lib/nxhdr.o"; cp $(BINFOLDER)mknx64 "$(SDK_TC)/bin/$(DROPBEAR_TRIPLE)-mknx"
else
DROPBEAR_TRIPLE   := i686-nanos
DROPBEAR_PORT_ENV  = -e 'CFLAGS=-Os -fno-pie -fno-PIC' -e 'LDFLAGS=-no-pie'
DROPBEAR_PREREQ    = @true
DROPBEAR_STARTUP   = true
endif
dropbear: bin/libc.ndl bin/libc.ndl.a
	@test -d "$(SDK_TC)/$(DROPBEAR_TRIPLE)/include" || { echo "nanos-sdk $(DROPBEAR_TRIPLE) toolchain not found at $(SDK_TC)"; exit 1; }
	@test -f "$(DROPBEAR_PORT)/nxport.toml"   || { echo "dropbear port not found at $(DROPBEAR_PORT)/nxport.toml"; exit 1; }
	# x86_64: (re)build the in-tree 64-bit libc/crt0/nxhdr/mknx first; i686 uses the prereqs as-is.
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
	@echo "staged $(BINFOLDER)dropbear.nxe (+ dropbearkey/dbclient) — run 'make image' (i686) or 'make image64' (x86_64) to install"

# busybox udhcpc (DHCP client, FAZA F). Reproducible like ping/wget: refresh the SDK sysroot from
# this checkout, then cross-build busybox configured with ONLY udhcpc (nanos-build.sh in the
# busybox tree) and mknx it. No busybox source patches — sysroot headers + EXTRA_CFLAGS only.
# The action helper (dhcpcfg.nxe) ships separately as /nanos/config/udhcpc.script via _image.
BB_DIR := $(SDK_WORK)/busybox-1.36.1
udhcpc: bin/libc.ndl bin/libc.ndl.a
	@test -f "$(BB_DIR)/nanos-build.sh" || { echo "busybox not set up at $(BB_DIR) (extract busybox-1.36.1 + nanos-build.sh)"; exit 1; }
	cp -R user/libc-glue/include/. "$(SDK_TC)/i686-nanos/include/"
	cp kernel/SyscallNr.h          "$(SDK_TC)/i686-nanos/include/SyscallNr.h"
	cp $(BINFOLDER)libc.ndl.a      "$(SDK_TC)/i686-nanos/lib/libc.a"
	cp $(BINFOLDER)libc.ndl        "$(SDK_TC)/i686-nanos/lib/libc.ndl"
	docker run --rm -v "$(SDK_WORK)":/work \
	  -e PATH="/work/toolchain/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
	  -w /work/busybox-1.36.1 nanos-sdk-dev:latest bash -c '\
	    test -f /work/toolchain/i686-nanos/lib/libm.a || i686-nanos-ar rcs /work/toolchain/i686-nanos/lib/libm.a; \
	    rm -f busybox busybox_unstripped networking/udhcp/built-in.o; \
	    bash nanos-build.sh && i686-nanos-mknx busybox_unstripped udhcpc.nxe --need libc.ndl'
	cp "$(BB_DIR)/udhcpc.nxe" $(BINFOLDER)udhcpc.nxe
	@echo "staged $(BINFOLDER)udhcpc.nxe — run 'make image' to install it into /nanos/bin"

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
# link probes) are refreshed first, like ping. i686 is unchanged (generic cross gcc, no shim).
ZLIB_PORT := $(SDK_WORK)/zlib-port
ifeq ($(ARCH),x86_64)
ZLIB_TRIPLE  := x86_64-nanos
ZLIB_PORT_ENV = -e NX_HOST=x86_64-nanos -e NX_LP64=1
ZLIB_PREREQ   = $(NXPORT_PREREQ)
ZLIB_STARTUP  = cp $(BINFOLDER)crt0.o "$(SDK_TC)/$(ZLIB_TRIPLE)/lib/crt0.o"; cp $(BINFOLDER)nxhdr.o "$(SDK_TC)/$(ZLIB_TRIPLE)/lib/nxhdr.o"; cp $(BINFOLDER)mknx64 "$(SDK_TC)/bin/$(ZLIB_TRIPLE)-mknx"
else
ZLIB_TRIPLE  := i686-nanos
ZLIB_PORT_ENV =
ZLIB_PREREQ   = @true
ZLIB_STARTUP  = true
endif
zlib: bin/libc.ndl bin/libc.ndl.a
	@test -d "$(SDK_TC)/$(ZLIB_TRIPLE)/include" || { echo "nanos-sdk $(ZLIB_TRIPLE) toolchain not found at $(SDK_TC)"; exit 1; }
	@test -f "$(ZLIB_PORT)/nxport.toml"   || { echo "zlib port not found at $(ZLIB_PORT)/nxport.toml"; exit 1; }
	# x86_64: (re)build the in-tree 64-bit libc/crt0/nxhdr/mknx first; i686 uses the prereqs as-is.
	$(ZLIB_PREREQ)
	cp -R user/libc-glue/include/. "$(SDK_TC)/$(ZLIB_TRIPLE)/include/"
	cp kernel/SyscallNr.h          "$(SDK_TC)/$(ZLIB_TRIPLE)/include/SyscallNr.h"
	# The dllimport shim (errno/stdin/stdout/stderr -> libc.ndl IAT slots). x86_64 code references
	# these DATA exports RIP-relative (R_X86_64_PC32), which mknx can't auto-import; pre_configure
	# -include's it into every TU via CFLAGS. i686 uses absolute relocs and skips it (NX_HOST != x86_64-nanos).
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
# --with-fallbacks=xterm-256color (no runtime terminfo DB shipped, matching the i686 port);
# --disable-db-install ships none. `make image`/`make image64` never depends on this.
#
# ARCH-AWARE (mirrors `make zlib`/`make ping`): for ARCH=x86_64 the port targets the x86_64-nanos
# sysroot, sets NX_HOST=x86_64-nanos + NX_LP64=1, and exports CFLAGS with the NanOS x86_64 user ABI
# (-fno-pie/-mcmodel=small/-mno-red-zone) for the cross-target compiles. The nx-dllimport.h data
# shim is appended to include/ncurses_cfg.h by hooks/post_configure.sh (guarded with USE_BUILD_CC so
# ncurses' host build-tools that compile the fallbacks never see it). The in-tree x86_64
# crt0/nxhdr/libc/mknx (link probes + final lib) are refreshed first, and the honest-conftest data
# stub regenerated (PRECMD) so autoconf's link probes detect functions faithfully. i686 is
# unchanged (generic cross gcc, absolute relocs, no shim, no extra CFLAGS).
NCURSES_PORT := $(SDK_WORK)/ncurses-port
ifeq ($(ARCH),x86_64)
NCURSES_TRIPLE := x86_64-nanos
NCURSES_PORT_ENV = -e NX_HOST=x86_64-nanos -e NX_LP64=1 -e CFLAGS="-O2 -fno-pie -mcmodel=small -mno-red-zone"
NCURSES_PREREQ   = $(NXPORT_PREREQ)
NCURSES_STARTUP  = cp $(BINFOLDER)crt0.o "$(SDK_TC)/$(NCURSES_TRIPLE)/lib/crt0.o"; cp $(BINFOLDER)nxhdr.o "$(SDK_TC)/$(NCURSES_TRIPLE)/lib/nxhdr.o"; cp $(BINFOLDER)mknx64 "$(SDK_TC)/bin/$(NCURSES_TRIPLE)-mknx"
NCURSES_PRECMD   = NM=$(NCURSES_TRIPLE)-nm CONFTEST_STUB_CC=$(NCURSES_TRIPLE)-gcc.real sh /work/toolchain/bin/gen-conftest-stubs.sh /work/toolchain/$(NCURSES_TRIPLE)/lib &&
else
NCURSES_TRIPLE := i686-nanos
NCURSES_PORT_ENV =
NCURSES_PREREQ   = @true
NCURSES_STARTUP  = true
NCURSES_PRECMD   =
endif
ncurses: bin/libc.ndl bin/libc.ndl.a
	@test -d "$(SDK_TC)/$(NCURSES_TRIPLE)/include" || { echo "nanos-sdk $(NCURSES_TRIPLE) toolchain not found at $(SDK_TC)"; exit 1; }
	@test -f "$(NCURSES_PORT)/nxport.toml" || { echo "ncurses port not found at $(NCURSES_PORT)/nxport.toml"; exit 1; }
	# x86_64: (re)build the in-tree 64-bit libc/crt0/nxhdr/mknx first; i686 uses the prereqs as-is.
	$(NCURSES_PREREQ)
	cp -R user/libc-glue/include/. "$(SDK_TC)/$(NCURSES_TRIPLE)/include/"
	cp kernel/SyscallNr.h          "$(SDK_TC)/$(NCURSES_TRIPLE)/include/SyscallNr.h"
	# The dllimport shim (stdout/stderr/errno -> libc.ndl IAT slots). x86_64 lib objects reference
	# these DATA exports RIP-relative (R_X86_64_PC32); the port's post_configure hook #includes it
	# into ncurses_cfg.h (only for x86_64; i686 uses absolute relocs and skips it).
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
# the manifest) so libpng uses the portable C path — matches the i686 port and avoids the QEMU-CPU
# SIMD trap. `make image`/`make image64` never depends on this.
#
# ARCH-AWARE (mirrors `make ncurses`/`make zlib`): for ARCH=x86_64 the port targets the x86_64-nanos
# sysroot, sets NX_HOST=x86_64-nanos + NX_LP64=1, and exports CFLAGS with the NanOS x86_64 user ABI
# (-fno-pie/-mcmodel=small/-mno-red-zone) for the cross-target compiles. The nx-dllimport.h data
# shim is appended to the generated config.h by hooks/post_configure.sh (guarded with USE_BUILD_CC),
# so libpng's RIP-relative reference to picolibc's `stderr` DATA becomes an __imp_stderr IAT slot a
# downstream app's mknx fills. The in-tree x86_64 crt0/nxhdr/libc/mknx (link probes + final lib) are
# refreshed first, and the honest-conftest data stub regenerated (PRECMD) so autoconf's link probes
# (AC_CHECK_LIB(z,...) etc.) detect symbols faithfully. i686 is unchanged (generic cross gcc, absolute
# relocs, no shim, no extra CFLAGS).
LIBPNG_PORT := $(SDK_WORK)/libpng-port
ifeq ($(ARCH),x86_64)
LIBPNG_TRIPLE := x86_64-nanos
LIBPNG_PORT_ENV = -e NX_HOST=x86_64-nanos -e NX_LP64=1 -e CFLAGS="-O2 -fno-pie -mcmodel=small -mno-red-zone"
LIBPNG_PREREQ   = $(NXPORT_PREREQ)
LIBPNG_STARTUP  = cp $(BINFOLDER)crt0.o "$(SDK_TC)/$(LIBPNG_TRIPLE)/lib/crt0.o"; cp $(BINFOLDER)nxhdr.o "$(SDK_TC)/$(LIBPNG_TRIPLE)/lib/nxhdr.o"; cp $(BINFOLDER)mknx64 "$(SDK_TC)/bin/$(LIBPNG_TRIPLE)-mknx"
LIBPNG_PRECMD   = NM=$(LIBPNG_TRIPLE)-nm CONFTEST_STUB_CC=$(LIBPNG_TRIPLE)-gcc.real sh /work/toolchain/bin/gen-conftest-stubs.sh /work/toolchain/$(LIBPNG_TRIPLE)/lib &&
else
LIBPNG_TRIPLE := i686-nanos
LIBPNG_PORT_ENV =
LIBPNG_PREREQ   = @true
LIBPNG_STARTUP  = true
LIBPNG_PRECMD   =
endif
libpng: bin/libc.ndl bin/libc.ndl.a
	@test -d "$(SDK_TC)/$(LIBPNG_TRIPLE)/include" || { echo "nanos-sdk $(LIBPNG_TRIPLE) toolchain not found at $(SDK_TC)"; exit 1; }
	@test -f "$(SDK_TC)/$(LIBPNG_TRIPLE)/lib/libz.a" || { echo "libz.a not in the $(LIBPNG_TRIPLE) sysroot — run 'make $(if $(filter x86_64,$(ARCH)),ARCH=x86_64 ,)zlib' first"; exit 1; }
	@test -f "$(LIBPNG_PORT)/nxport.toml" || { echo "libpng port not found at $(LIBPNG_PORT)/nxport.toml"; exit 1; }
	# x86_64: (re)build the in-tree 64-bit libc/crt0/nxhdr/mknx first; i686 uses the prereqs as-is.
	$(LIBPNG_PREREQ)
	cp -R user/libc-glue/include/. "$(SDK_TC)/$(LIBPNG_TRIPLE)/include/"
	cp kernel/SyscallNr.h          "$(SDK_TC)/$(LIBPNG_TRIPLE)/include/SyscallNr.h"
	# The dllimport shim (stderr/errno -> libc.ndl IAT slots). x86_64 lib objects reference these DATA
	# exports RIP-relative (R_X86_64_PC32); the port's post_configure hook #includes it into config.h
	# (only for x86_64; i686 uses absolute relocs and skips it).
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
# the i686 port and avoids the hand-written NASM SIMD path that has no NanOS ABI. `make image`/
# `make image64` never depends on this.
#
# ARCH-AWARE (mirrors `make libpng`/`make ncurses`): for ARCH=x86_64 the port targets the x86_64-nanos
# sysroot and sets NX_HOST=x86_64-nanos + NX_LP64=1. Unlike the autotools ports (which pass the ABI
# cflags via -e CFLAGS), the CMake ABI/cflags + the nx-dllimport.h DATA-import shim are injected by
# the NX_HOST-aware cmake toolchain file itself (CMAKE_C_FLAGS_INIT for x86_64), so a downstream app
# linking libjpeg.a binds picolibc's `stderr` (jerror.c's default handlers) through the libc.ndl IAT.
# The in-tree x86_64 crt0/nxhdr/libc/mknx are refreshed first (cmake links the cjpeg/djpeg helper
# binaries). i686 is unchanged (generic cross gcc, absolute relocs, no shim, no extra CFLAGS).
LIBJPEG_PORT := $(SDK_WORK)/libjpeg-port
ifeq ($(ARCH),x86_64)
LIBJPEG_TRIPLE := x86_64-nanos
LIBJPEG_PORT_ENV = -e NX_HOST=x86_64-nanos -e NX_LP64=1
LIBJPEG_PREREQ   = $(NXPORT_PREREQ)
LIBJPEG_STARTUP  = cp $(BINFOLDER)crt0.o "$(SDK_TC)/$(LIBJPEG_TRIPLE)/lib/crt0.o"; cp $(BINFOLDER)nxhdr.o "$(SDK_TC)/$(LIBJPEG_TRIPLE)/lib/nxhdr.o"; cp $(BINFOLDER)mknx64 "$(SDK_TC)/bin/$(LIBJPEG_TRIPLE)-mknx"
else
LIBJPEG_TRIPLE := i686-nanos
LIBJPEG_PORT_ENV =
LIBJPEG_PREREQ   = @true
LIBJPEG_STARTUP  = true
endif
libjpeg: bin/libc.ndl bin/libc.ndl.a
	@test -d "$(SDK_TC)/$(LIBJPEG_TRIPLE)/include" || { echo "nanos-sdk $(LIBJPEG_TRIPLE) toolchain not found at $(SDK_TC)"; exit 1; }
	@test -f "$(LIBJPEG_PORT)/nxport.toml" || { echo "libjpeg port not found at $(LIBJPEG_PORT)/nxport.toml"; exit 1; }
	# x86_64: (re)build the in-tree 64-bit libc/crt0/nxhdr/mknx first; i686 uses the prereqs as-is.
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

# NetSurf graphical web browser (optional, external): a full stack of ported libraries (zlib,
# libpng/jpeg, libcurl over the ported OpenSSL, libcss/libdom/libhubbub/...) + the bespoke nanowm
# libnsfb surface backend, all in the separate netsurf-nanos repo. `make netsurf` refreshes the SDK
# sysroot from this checkout, then runs that repo's ordered build-all (each port cross-builds in the
# nanos-sdk-dev container — build-all runs on the HOST and shells into Docker per port, so it is NOT
# run inside a container here), and stages netsurf.nxe + its res/ tree into bin/. `make image`
# installs the /apps/netsurf bundle. Launch it inside nanowm with `-f nanwm`.
NETSURF_REPO ?= $(HOME)/Projects/netsurf-nanos
netsurf: bin/libc.ndl bin/libc.ndl.a bin/libnw.ndl bin/libnw.ndl.a
	@test -f "$(NETSURF_REPO)/scripts/build-all.sh" || { echo "netsurf-nanos repo not found at $(NETSURF_REPO)"; exit 1; }
	sh "$(NETSURF_REPO)/scripts/sync-sysroot.sh"
	sh "$(NETSURF_REPO)/scripts/build-all.sh"
	cp "$(NETSURF_REPO)/ports/netsurf/netsurf.nxe" $(BINFOLDER)netsurf.nxe
	rm -rf $(BINFOLDER)netsurf-res && cp -R "$(NETSURF_REPO)/ports/netsurf/res" $(BINFOLDER)netsurf-res
	@echo "staged $(BINFOLDER)netsurf.nxe + res — run 'make image' to install /apps/netsurf"

# Stage EVERY already-built external app into bin/ in one go (best-effort: skips any whose artifact
# is not present, so a partial set still works). The staged .nxe are build artifacts that `make
# clean` removes, so the workflow after a clean is: `make externals && make image`. This copies
# prebuilt binaries only — it does NOT rebuild them (use `make bash`/the nanos-sdk for that).
externals:
	@n=0; \
	for spec in "bash:$(BASH_FORK)/nanos/bash.nxe" \
	            "vim:$(SDK_WORK)/vim/src/vim.nxe" \
	            "grep:$(SDK_WORK)/grep-3.11/src/grep.nxe" \
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
	echo "staged $$n external app(s) into $(BINFOLDER) — run 'make image' to install them"

# Desktop artwork: convert the branded PNGs (NanOS wallpaper + logo) to NanOS's flat 32bpp surface
# format on the host (needs python3 + Pillow), staged into bin/ where _image installs them under
# /nanos/share. The compositor uses wallpaper.raw as the desktop background and About shows logo.raw;
# both fall back gracefully if absent. Source PNGs live in assets/ (override with ART_DIR=).
ART_DIR ?= $(CURDIR)/assets
.PHONY: assets externals bash grep vim bzip2 ping wget git inetd httpd udhcpc zlib ncurses libpng libjpeg htop libdrm   # never confuse these with the assets/ dir or bin/ files
assets:
	@command -v python3 >/dev/null 2>&1 || { echo "need python3 + Pillow for assets"; exit 1; }
	python3 scripts/png2raw.py "$(ART_DIR)/wallpaper.png" $(BINFOLDER)wallpaper.raw 1024x768 --bg 0x0a1020
	python3 scripts/png2raw.py "$(ART_DIR)/logo.png"      $(BINFOLDER)logo.raw      96x96     --bg 0xf4f8fd
	@echo "staged $(BINFOLDER)wallpaper.raw + logo.raw — run 'make image' to install them"

# -cpu Nehalem: expose RDRAND so the kernel CSPRNG seeds from a hardware RNG (the default qemu32
# CPU lacks it — without this flag archHwRandom returns false and the seed is RDTSC-jitter+RTC only).
# Default set by arch/$(ARCH)/arch.mk (i686 -> Nehalem, x86_64 -> qemu64); ?= lets the arch value
# (assigned at the line 4 include, before this) win.
QEMU_CPU ?= -cpu Nehalem
# RAM: 512 MiB. The kernel reads the real size from multiboot and lays out its windows above it
# (mmu_x86.cpp), so this is just the QEMU knob — bump it freely (up to ~1 GiB with the current
# window placement). More RAM = bigger kernel heap + a bigger user frame pool.
QEMU_MEM=-m 512

# The x86_64 emulator + CPU, INDEPENDENT of the selected ARCH. The `run64`/`bringup64` targets are
# inherently 64-bit, but they are usually invoked as plain `make run64` (no ARCH=x86_64), so `$(QEMU)`
# / `$(QEMU_CPU)` would resolve from the DEFAULT (i686) arch.mk to qemu-system-i386 — which loads the
# 64-bit kernel, faults entering long mode, triple-faults and reboots into an endless GRUB loop. Pin
# the 64-bit emulator here so these targets always boot the x86_64 image with the right machine.
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

run: image
	$(QEMU) $(QEMU_CPU) $(QEMU_MEM) -drive file=$(IMAGE_GRUB2),format=raw $(NIC_NET)

run-iso: iso
	$(QEMU) -cdrom nanos.iso

# Networking harness (FAZA 0 of docs/superpowers/plans/2026-06-12-networking.md). Attach an
# Intel e1000 (82540EM = PCI 8086:100E — a REAL NIC with the canonical Linux driver) on QEMU's
# user-mode NAT and dump EVERY frame (RX+TX) to a pcap. NIC_OPTS is shared by run-net and
# scripts/net-capture.sh so the wire we observe is exactly the wire we ship. The NAT hands the
# guest 10.0.2.15 (gateway 10.0.2.2, DNS forwarder 10.0.2.3, DHCP server 10.0.2.2); traffic
# reaches the real internet through the host, so `ping wp.pl` can actually go out. The pcap is
# the "no-shortcuts" gate: we byte-compare our ARP/IP/ICMP/DNS/TCP against real Linux. hostfwd
# (host:5555 -> guest:80) lets a host client reach a guest server for loopback-free tests.
PCAP ?= /tmp/nanos.pcap
# hostfwd map (host port -> guest service): 5555->80 (httpd), 2323->23 (telnetd), 5007->7 (echo),
# 5013->13 (daytime), 5443->5443 (openssl s_server TLS), 2222->22 (sshd) — the inetd built-ins +
# services + the TLS/SSH servers let a host client reach the guest servers.
# NIC_NET = the NIC + NAT + host port-forwards. `make run` uses it so a host `telnet localhost 2323`
# reaches the guest's telnetd out of the box (no extra flags). run-net adds a filter-dump pcap.
NIC_NET=-netdev user,id=n0,hostfwd=tcp::5555-:80,hostfwd=tcp::2323-:23,hostfwd=tcp::5007-:7,hostfwd=tcp::5013-:13,hostfwd=tcp::5443-:5443,hostfwd=tcp::2222-:22 \
        -device e1000,netdev=n0
NIC_OPTS=$(NIC_NET) -object filter-dump,id=d0,netdev=n0,file=$(PCAP)

run-net: image
	$(QEMU) $(QEMU_CPU) $(QEMU_MEM) -drive file=$(IMAGE_GRUB2),format=raw $(NIC_OPTS)

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
.PHONY: test64 smoke-x86_64 smoke-uefi smoke-bigmem smoke-e1000e smoke-usb smoke-usb-smp smoke-usb-dmawindow smoke-vt smoke-smp verify64
test64: test-image
	$(TEST_DOCKER_RUN) make ARCH=x86_64 _test

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

# `verify64` = the full x86_64 gate: host tests + BIOS + UEFI + big-RAM + e1000e MSI-X + live-USB + SMP smokes.
verify64: test64 smoke-x86_64 smoke-uefi smoke-bigmem smoke-e1000e smoke-usb smoke-usb-smp smoke-usb-dmawindow smoke-vt smoke-virtio-gpu smoke-smp smoke-smp-speedup smoke-smp-stress smoke-smp-netstress
	@echo "x86_64 verify: host tests + BIOS + UEFI + big-RAM + e1000e MSI + live-USB + live-USB+SMP + VT switch + virtio-gpu (unmodified DRM) + SMP boot + SMP speedup + SMP data-race (stress/netstress) gates all passed."

clean:
	$(DOCKER_RUN) make _clean
	-rm -rf iso/ nanos.iso $(IMAGE_GRUB2) coverage/

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
# stack linked in) and install it into a GRUB2 ext4 disk image as /nanos/core/kernel64.bin, with
# a grub.cfg that multiboots it. Unlike bringup64 (rescue ISO) this boots from a real -drive disk
# whose ext4 partition the staged kernel mounts at /disks/main and reads/writes.
.PHONY: image64
image64:
	$(DOCKER_RUN) make ARCH=x86_64 _image64

run64: image64
	$(QEMU64) $(QEMU_CPU64) $(QEMU_SMP64) $(QEMU_MEM) -drive file=$(IMAGE64_GRUB2),format=raw $(QEMU_DISPLAY64) $(NIC_NET)

# GL/virgl interactive run — like `run64`, but with GPU-accelerated OpenGL ES. Stock homebrew QEMU
# has no virgl, so `run64` cannot show GL; this points at the kosmickrisp fork (virtio-gpu-gl →
# virglrenderer → ANGLE → Metal on Apple Silicon) and adds the cocoa GL display. Boot, log in
# root/nanos (or jan/jan on the graphics VT F7), and the nwm desktop renders GPU-accelerated.
# Override the binary path with QEMU_GL=/path/to/qemu-system-x86_64 if the fork moved. The fork is
# validated single-vCPU (MTTCG-SMP + virgl is flaky), so this pins -smp 1 regardless of NCPU64.
QEMU_GL     ?= $(HOME)/Projects/nanos-sdk-work/qemu-virgl-kosmickrisp/bin/qemu-system-x86_64
# Use the PURE virtio-gpu-gl device, NOT virtio-vga-gl: virtio-vga-gl also exposes a legacy VGA
# output that QEMU displays by DEFAULT, so the cocoa gl=es window shows the (black, unused) VGA
# surface and the virtio-gpu scanout — the actual desktop — never appears. virtio-gpu-gl-pci has no
# VGA part, so the virtio-gpu scanout IS the display and the desktop shows. (A/B-proven: with
# virtio-vga-gl the window is black; with virtio-gpu-gl-pci the full nwm desktop composites.)
QEMU_GL_VGA ?= -device virtio-gpu-gl-pci -display cocoa,gl=es,zoom-to-fit=on
# No NIC by default: the kosmickrisp fork is built WITHOUT the slirp ('user') network backend, so
# passing NIC_NET aborts it ("network backend 'user' is not compiled into this binary"). GL bring-up
# needs no network. To add one anyway, build the fork with slirp and run `make run64-gl QEMU_GL_NET='...'`.
QEMU_GL_NET ?=
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
	    -drive file=$(IMAGE64_GRUB2),format=raw $(QEMU_GL_VGA) -no-reboot $(QEMU_GL_NET)

# Headless GL proof — boot on the virgl fork, log in, run `gles2info`, print the GL markers to this
# terminal (no cocoa window, no clicking). Success = `renderer=virgl`. The GL-composited desktop is
# a separate unbuilt milestone; this proves only the unmodified-Mesa → virgl → ANGLE → Metal path.
.PHONY: run64-gl-test
run64-gl-test: image64
	QEMU_GL="$(QEMU_GL)" IMG="$(IMAGE64_GRUB2)" bash scripts/run64-gl-selftest.sh

# Doom (in-tree doomgeneric), ARCH-AWARE host wrapper. Stages bin/doom.nxe in the container for
# the selected arch — i686 (default) or x86_64 — using the arch-selected userland toolchain,
# picolibc sysroot, linker script and mknx (see the $(BINFOLDER)doom.nxe rule). For i686 doom is
# also built by `make image` (it is in USER_PROGS); for x86_64 it is NOT in the minimal
# X64_USER_PROGS subset, so this target stages it and `make image64` then installs the /apps/doom
# bundle + /bin/doom.nxe symlink. Switching ARCH reuses bin/ for userland objects, so run
# `make clean` when crossing arches (the documented x86_64 convention) before `make ARCH=x86_64 doom`.
.PHONY: doom
doom:
	$(DOCKER_RUN) sh -c 'make -j"$$(nproc)" ARCH=$(ARCH) doom'
	@echo "staged $(BINFOLDER)doom.nxe (ARCH=$(ARCH)) — run 'make image' (i686) or 'make image64' (x86_64) to install /apps/doom"

# x86_64 minimal userland (Plan 6): build the 64-bit init.nxe in the container (crt0 +
# nxhdr + libnanos + init, linked at 0x800000, then mknx64 -> v4 .nxe). Host-side wrapper.
.PHONY: init64
init64:
	$(DOCKER_RUN) make ARCH=x86_64 bin/init.nxe

# Plan 10 Tasks 5-6: the x86_64 libc + pthread/TLS ring-3 smokes. Builds a real picolibc program
# (printf) and a pthread/TLS program (errno + a thread) with the x86_64-nanos SDK toolchain
# against the SDK sysroot, proving the 4-artifact libc contract (libc.ndl + import lib + crt0 +
# nxhdr) and the pthread arch port run in ring 3. To run one: install it as PID 1 (overwrite
# /nanos/core/init.nxe in the image) and boot. The sysroot-inject below mirrors the i686
# `make <app>` cp lines, arch-selected via NANOS_TRIPLE (= x86_64-nanos when ARCH=x86_64).
NANOS_TRIPLE := $(if $(filter x86_64,$(ARCH)),x86_64-nanos,i686-nanos)
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
	#    i686 bin/*.o into the x86_64 link (the libc-glue objects are not arch-suffixed).
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
# Tools resolved from the container PATH (i686-elf toolchain in /opt/cross).
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
# Not -O3: keeps NXE/kernel size + behaviour predictable (i686-elf gcc has no SSE to vectorise into).
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

# Kernel objects live in a PER-ARCH subdirectory so the elf32 (i686) and elf64 (x86_64)
# objects of the same basename (Kernel.o, Exec.o, ...) never collide in bin/. Switching ARCH
# without a clean would otherwise mix ELF classes (stale elf32 objects linked into an elf64
# kernel → mangling/format mismatches). i686 keeps bin/ (unchanged); x86_64 uses bin/k64/.
# The final kernel.bin still lands at bin/kernel.bin (only one arch is current at a time).
ifeq ($(ARCH),x86_64)
KOBJ=$(BINFOLDER)k64/
else
KOBJ=$(BINFOLDER)
endif

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

# -lgcc trails the objects so libgcc helper routines first referenced by an object (e.g.
# __udivdi3 for 64-bit division on i686 — pulled in by the LP64 widening of Plan 7) resolve.
# (LDFLAGS also lists -lgcc, but a library only satisfies symbols undefined to its left.)
# kernel.bin lands in the per-arch $(KOBJ) too (bin/kernel.bin for i686, bin/k64/kernel.bin for
# x86_64) so the two arches' final binaries never overwrite each other in bin/.
$(KOBJ)kernel.bin: $(OBJECTS)
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS) -lgcc

# Kernel object rules write into the per-arch $(KOBJ) (bin/ for i686, bin/k64/ for x86_64).
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
# bin/kernel.bin). Then: (re)create the disk skeleton (create-grub2-image.sh, IMAGE_PATH
# overridden to the x86_64 image), point grub.cfg at the real kernel, and write it to
# /nanos/core/kernel.bin. The ext4 partition doubles as /disks/main, which the real kernel
# mounts at boot; init.nxe (PID 1) is installed at /nanos/core/init.nxe — the exact path
# kernel/Kernel.cpp execs. (The staged kernel + its disk path are retired here; bringup64
# remains as the staged rescue-ISO smoke target.)
_image64: _all _userland64 _kext
	# Hybrid GPT image bootable under BOTH BIOS and UEFI via Limine (limine.conf on the ESP points at
	# /nanos/core/kernel.bin on the ext4 root by label — no /boot/grub/grub.cfg needed).
	IMAGE_PATH=$(IMAGE64_GRUB2) NANOS_BOOT=limine ./scripts/create-grub2-image.sh
	# System volume skeleton (mirror i686 _image): /nanos/{core,bin,lib,kext,config,cache,logs,
	# share/terminfo/x} + the /apps bundle root + the /bin link farm, created upfront so every
	# subsequent install step (and the optional-app blocks below) finds its parent directory.
	-printf "mkdir /nanos\nmkdir /nanos/core\nmkdir /nanos/bin\nmkdir /nanos/lib\nmkdir /nanos/kext\nmkdir /nanos/config\nmkdir /nanos/cache\nmkdir /nanos/logs\nmkdir /nanos/share\nmkdir /nanos/share/icons\nmkdir /nanos/share/terminfo\nmkdir /nanos/share/terminfo/x\nmkdir /apps\nmkdir /bin\n" | debugfs -w "$(IMAGE64_GRUB2_PART)" 2>/dev/null
	printf "rm /nanos/core/kernel.bin\nwrite $(KOBJ)kernel.bin /nanos/core/kernel.bin\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"
	# Loadable kernel modules (.nkext) -> /nanos/kext; the kernel scans + loads them at boot
	# (loadAllKexts). The PS/2 keyboard + mouse + e1000 NIC drivers live here, NOT in kernel.bin.
	for m in $(KEXTS); do \
	  printf "rm /nanos/kext/$$m.nkext\nwrite $(BINFOLDER)$$m.nkext /nanos/kext/$$m.nkext\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"; \
	done
	# PID 1: the real user/init.c (dynamically linked against libc.ndl), which execve()s the
	# login shell. The kernel execs /disks/main/nanos/core/init.nxe and runs it in ring 3.
	printf "rm /nanos/core/init.nxe\nwrite $(BINFOLDER)init.nxe /nanos/core/init.nxe\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"
	printf "set_inode_field /nanos/core/init.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"   # executables need +x (exec-perm enforced)
	# The shared C library the dynamic loader binds every program against -> /nanos/lib.
	printf "rm /nanos/lib/libc.ndl\nwrite $(BINFOLDER)libc.ndl /nanos/lib/libc.ndl\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"
	# The shell + the sbase coreutils -> /nanos/bin (nsh resolves a bare command name here first).
	for p in $(X64_SYS_PROGS); do \
	  printf "rm /nanos/bin/$$p.nxe\nwrite $(BINFOLDER)$$p.nxe /nanos/bin/$$p.nxe\nset_inode_field /nanos/bin/$$p.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"; \
	done
	# chsh edits the account DB, so it runs setuid-root (a non-root user changing their own shell).
	printf "set_inode_field /nanos/bin/chsh.nxe mode 0104755\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"
	# nanosu is the "authenticate to open" privileged helper: it verifies root's password and runs the
	# target as root, so it must be setuid-root (owner root, mode 04755).
	printf "set_inode_field /nanos/bin/nanosu.nxe mode 0104755\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"
	# nanowm compositor (a system GUI program) -> /nanos/bin, and the nanowm client shared libs
	# (libnw.ndl / libnwui.ndl) -> /nanos/lib (the NetSurf libnsfb backend binds libnw.ndl at load).
	for p in $(X64_GUI_PROGS); do \
	  printf "rm /nanos/bin/$$p.nxe\nwrite $(BINFOLDER)$$p.nxe /nanos/bin/$$p.nxe\nset_inode_field /nanos/bin/$$p.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"; \
	done
	for l in $(X64_GUI_LIBS); do \
	  printf "rm /nanos/lib/$$l\nwrite $(BINFOLDER)$$l /nanos/lib/$$l\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"; \
	done
	# nanowm desktop apps -> /apps/<name>/<name>.nxe bundles + /bin/<name>.nxe symlink (the link farm),
	# the layout nwm spawns them from. Mirrors the i686 APP_PROGS loop.
	for p in $(X64_GUI_APPS); do \
	  printf "mkdir /apps/$$p\n" | debugfs -w "$(IMAGE64_GRUB2_PART)" 2>/dev/null; \
	  printf "rm /apps/$$p/$$p.nxe\nwrite $(BINFOLDER)$$p.nxe /apps/$$p/$$p.nxe\nset_inode_field /apps/$$p/$$p.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"; \
	  printf "rm /bin/$$p.nxe\n" | debugfs -w "$(IMAGE64_GRUB2_PART)" 2>/dev/null; \
	  printf "symlink /bin/$$p.nxe /apps/$$p/$$p.nxe\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"; \
	done
	# Desktop artwork. The wallpaper ships as the source PNG; nanowm decodes it and cover-fits it to
	# the live resolution at runtime (so it fills ANY panel). The logo stays a fixed 96x96 raw (About
	# blits it directly, no scaling). Mirrors the i686 _image artwork block.
	if [ -f assets/wallpaper.png ]; then \
	  printf "rm /nanos/share/wallpaper.png\nwrite assets/wallpaper.png /nanos/share/wallpaper.png\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"; \
	fi
	if [ -f $(BINFOLDER)logo.raw ]; then \
	  printf "rm /nanos/share/logo.raw\nwrite $(BINFOLDER)logo.raw /nanos/share/logo.raw\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"; \
	fi
	# Icon set for the Rust file explorer (rsexp) -> /nanos/share/icons. Decoded at runtime via the
	# toolkit PNG loader; the iconview color-keys magenta as transparent.
	for ic in computer drive folder home program text image file ui-back ui-fwd ui-up ui-home; do \
	  if [ -f assets/icons/$$ic.png ]; then \
	    printf "rm /nanos/share/icons/$$ic.png\nwrite assets/icons/$$ic.png /nanos/share/icons/$$ic.png\n" | debugfs -w "$(IMAGE64_GRUB2_PART)" 2>/dev/null; \
	  fi; \
	done
	# UI/terminal fonts (TTF) -> /nanos/share/fonts; nw_gfx loads the UI font (proportional, AA).
	-printf "mkdir /nanos/share/fonts\n" | debugfs -w "$(IMAGE64_GRUB2_PART)" 2>/dev/null
	for ft in UISans-Regular Mono-Regular; do \
	  if [ -f assets/fonts/$$ft.ttf ]; then \
	    printf "rm /nanos/share/fonts/$$ft.ttf\nwrite assets/fonts/$$ft.ttf /nanos/share/fonts/$$ft.ttf\n" | debugfs -w "$(IMAGE64_GRUB2_PART)" 2>/dev/null; \
	  fi; \
	done
	# Account database -> /nanos/config (init's getpwuid reads pw_shell from here; absent -> nsh).
	# passwd (x in field 2), shadow (hashes, 0600 root), group (with members), sudoers (%wheel).
	# The kernel copies all four into the writable /etc tmpfs at boot (Kernel.cpp populateEtc).
	printf "rm /nanos/config/passwd\nwrite config/passwd /nanos/config/passwd\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"
	printf "rm /nanos/config/shadow\nwrite config/shadow /nanos/config/shadow\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"
	printf "rm /nanos/config/group\nwrite config/group /nanos/config/group\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"
	printf "rm /nanos/config/sudoers\nwrite config/sudoers /nanos/config/sudoers\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"
	-printf "set_inode_field /nanos/config/shadow mode 0100600\nset_inode_field /nanos/config/sudoers mode 0100440\n" | debugfs -w "$(IMAGE64_GRUB2_PART)" 2>/dev/null
	# jan's home directory under /users (NanOS/macOS layout, not Linux /home), uid/gid 1000 so the
	# login shell can write there. Remove any stale /home from an earlier (incremental) build.
	-printf "rmdir /home/jan\nrmdir /home\n" | debugfs -w "$(IMAGE64_GRUB2_PART)" 2>/dev/null
	-printf "mkdir /users\nmkdir /users/jan\n" | debugfs -w "$(IMAGE64_GRUB2_PART)" 2>/dev/null
	-printf "set_inode_field /users/jan uid 1000\nset_inode_field /users/jan gid 1000\nset_inode_field /users/jan mode 040755\n" | debugfs -w "$(IMAGE64_GRUB2_PART)" 2>/dev/null
	# Network/login config templates -> /nanos/config/etc (kernel copies them into the writable /etc
	# tmpfs at boot, see Kernel.cpp populateEtc). /etc/shells in particular lists the valid login
	# shells: dropbear's getusershell() rejects an SSH login whose passwd shell isn't there. Mirrors
	# the i686 _image etc population.
	-printf "mkdir /nanos/config/etc\n" | debugfs -w "$(IMAGE64_GRUB2_PART)" 2>/dev/null
	for f in resolv.conf hosts nsswitch.conf protocols services inetd.conf shells profile; do \
	  printf "rm /nanos/config/etc/$$f\nwrite config/etc/$$f /nanos/config/etc/$$f\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"; \
	done
	# Per-user interactive bash config: seed jan's ~/.bashrc from config/skel (Linux /etc/skel).
	printf "rm /users/jan/.bashrc\nwrite config/skel/.bashrc /users/jan/.bashrc\nset_inode_field /users/jan/.bashrc uid 1000\nset_inode_field /users/jan/.bashrc gid 1000\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"
	# GNU bash (optional): installed as an /apps/bash bundle + a /bin/bash.nxe symlink ONLY if
	# `make ARCH=x86_64 bash` staged bin/bash.nxe. passwd's login shell is /disks/main/bin/bash.nxe,
	# so this is what PID 1 execve()s. Mirrors the i686 _image bash population. Skipped silently
	# otherwise (init falls back to nsh). The /apps + /bin link-farm dirs are created here.
	-printf "mkdir /apps\nmkdir /bin\n" | debugfs -w "$(IMAGE64_GRUB2_PART)" 2>/dev/null
	if [ -f $(BINFOLDER)bash.nxe ]; then \
	  printf "mkdir /apps/bash\n" | debugfs -w "$(IMAGE64_GRUB2_PART)" 2>/dev/null; \
	  printf "rm /apps/bash/bash.nxe\nwrite $(BINFOLDER)bash.nxe /apps/bash/bash.nxe\nset_inode_field /apps/bash/bash.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"; \
	  printf "rm /bin/bash.nxe\n" | debugfs -w "$(IMAGE64_GRUB2_PART)" 2>/dev/null; \
	  printf "symlink /bin/bash.nxe /apps/bash/bash.nxe\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"; \
	fi
	# grep + bzip2 (optional, external): system utilities -> /nanos/bin, installed only if
	# `make ARCH=x86_64 grep|bzip2` staged them. Mirrors the i686 _image population.
	if [ -f $(BINFOLDER)grep.nxe ]; then \
	  printf "rm /nanos/bin/grep.nxe\nwrite $(BINFOLDER)grep.nxe /nanos/bin/grep.nxe\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"; \
	fi
	if [ -f $(BINFOLDER)bzip2.nxe ]; then \
	  printf "rm /nanos/bin/bzip2.nxe\nwrite $(BINFOLDER)bzip2.nxe /nanos/bin/bzip2.nxe\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"; \
	fi
	# htop (optional, external): interactive process monitor built by `make ARCH=x86_64 htop` (the
	# nanos-sdk port), staged into bin/htop.nxe. A system utility (flat in /nanos/bin). Skipped if absent.
	if [ -f $(BINFOLDER)htop.nxe ]; then \
	  printf "rm /nanos/bin/htop.nxe\nwrite $(BINFOLDER)htop.nxe /nanos/bin/htop.nxe\nset_inode_field /nanos/bin/htop.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"; \
	fi
	# gles2info (optional, external): the Mesa (gallium-virgl + EGL + GLES2) bring-up oracle, built by
	# `make gles2info` (links the Mesa .a closure from the mesa-port). A system utility (flat in
	# /nanos/bin). Skipped if absent — only present on a GL dev build.
	if [ -f $(BINFOLDER)gles2info.nxe ]; then \
	  printf "rm /nanos/bin/gles2info.nxe\nwrite $(BINFOLDER)gles2info.nxe /nanos/bin/gles2info.nxe\nset_inode_field /nanos/bin/gles2info.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"; \
	fi
	# SQLite (optional, external): the real sqlite3 CLI built by `make ARCH=x86_64 sqlite` (the SQLite
	# fork). A system utility (flat in /nanos/bin); the libsqlite.ndl shared engine goes to /nanos/lib
	# so other apps can import the SQL API by name. Skipped if absent.
	if [ -f $(BINFOLDER)sqlite3.nxe ]; then \
	  printf "rm /nanos/bin/sqlite3.nxe\nwrite $(BINFOLDER)sqlite3.nxe /nanos/bin/sqlite3.nxe\nset_inode_field /nanos/bin/sqlite3.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"; \
	fi
	if [ -f $(BINFOLDER)libsqlite.ndl ]; then \
	  printf "rm /nanos/lib/libsqlite.ndl\nwrite $(BINFOLDER)libsqlite.ndl /nanos/lib/libsqlite.ndl\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"; \
	fi
	# toybox (optional, external): the user-identity multicall built by `make ARCH=x86_64 toybox`.
	# Installed as ONE setuid-root binary (mode 04755) with a per-command symlink farm — toybox's
	# CONFIG_TOYBOX_SUID drops privilege for the non-suid applets (id/groups/whoami) while
	# login/su/passwd keep root to read /etc/shadow + switch identity. Skipped if absent.
	if [ -f $(BINFOLDER)toybox.nxe ]; then \
	  printf "rm /nanos/bin/toybox.nxe\nwrite $(BINFOLDER)toybox.nxe /nanos/bin/toybox.nxe\nset_inode_field /nanos/bin/toybox.nxe mode 0104755\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"; \
	  for c in login su passwd id groups whoami; do \
	    printf "rm /nanos/bin/$$c.nxe\nln /nanos/bin/toybox.nxe /nanos/bin/$$c.nxe\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"; \
	  done; \
	fi
	# sudo (optional, external): real sudo built by `make ARCH=x86_64 sudo`. Installed setuid-root
	# (mode 04755) so an unprivileged user can escalate; it reads /etc/sudoers (%wheel, seeded).
	if [ -f $(BINFOLDER)sudo.nxe ]; then \
	  printf "rm /nanos/bin/sudo.nxe\nwrite $(BINFOLDER)sudo.nxe /nanos/bin/sudo.nxe\nset_inode_field /nanos/bin/sudo.nxe mode 0104755\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"; \
	fi
	# ping (optional, external): GNU inetutils ping built by `make ARCH=x86_64 ping` (the nanos-sdk
	# port) and staged into bin/ping.nxe. A system utility (flat in /nanos/bin). Skipped if absent.
	# Mirrors the i686 _image population.
	if [ -f $(BINFOLDER)ping.nxe ]; then \
	  printf "rm /nanos/bin/ping.nxe\nwrite $(BINFOLDER)ping.nxe /nanos/bin/ping.nxe\nset_inode_field /nanos/bin/ping.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"; \
	fi
	# openssl (optional, external): OpenSSL CLI built by `make ARCH=x86_64 openssl` (the nanos-sdk
	# port), staged into bin/openssl.nxe. A system utility (flat in /nanos/bin). Skipped if absent.
	# Mirrors the i686 _image population.
	if [ -f $(BINFOLDER)openssl.nxe ]; then \
	  printf "rm /nanos/bin/openssl.nxe\nwrite $(BINFOLDER)openssl.nxe /nanos/bin/openssl.nxe\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"; \
	fi
	# CA trust store + config (Mozilla bundle): OpenSSL's compiled OPENSSLDIR is /disks/main/nanos/ssl;
	# ship cert.pem + openssl.cnf there so the TLS clients can verify chains without a per-command
	# -CAfile and the CLI finds its config. Mirrors the i686 _image population. Skipped if absent.
	if [ -f disk-content/ssl/cert.pem ]; then \
	  printf "mkdir /nanos/ssl\n" | debugfs -w "$(IMAGE64_GRUB2_PART)" 2>/dev/null; \
	  printf "rm /nanos/ssl/cert.pem\nwrite disk-content/ssl/cert.pem /nanos/ssl/cert.pem\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"; \
	  printf "rm /nanos/ssl/openssl.cnf\nwrite disk-content/ssl/openssl.cnf /nanos/ssl/openssl.cnf\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"; \
	fi
	# Dropbear SSH (optional, external): server + keygen + client -> /nanos/bin, installed only if
	# `make ARCH=x86_64 dropbear` staged them. init's start_sshd() generates a persistent ed25519
	# host key under /nanos/config on first boot and launches `dropbear -r .. -p 22`; login is the
	# passwd shell (bash) over the kernel PTY. Also create root's home + .ssh (pubkey authorized_keys
	# location). Mirrors the i686 _image dropbear population. The host reaches sshd via hostfwd 2222->22.
	if [ -f $(BINFOLDER)dropbear.nxe ]; then \
	  for b in dropbear dropbearkey dbclient; do \
	    test -f $(BINFOLDER)$$b.nxe && printf "rm /nanos/bin/$$b.nxe\nwrite $(BINFOLDER)$$b.nxe /nanos/bin/$$b.nxe\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"; \
	  done; \
	  printf "mkdir /root\nmkdir /root/.ssh\n" | debugfs -w "$(IMAGE64_GRUB2_PART)" 2>/dev/null; \
	fi
	# vim (optional, external): an /apps/vim bundle + a /bin/vim.nxe symlink + its runtime
	# defaults.vim, only if `make ARCH=x86_64 vim` staged it. Mirrors the i686 _image bundle.
	if [ -f $(BINFOLDER)vim.nxe ]; then \
	  printf "mkdir /apps/vim\n" | debugfs -w "$(IMAGE64_GRUB2_PART)" 2>/dev/null; \
	  printf "rm /apps/vim/vim.nxe\nwrite $(BINFOLDER)vim.nxe /apps/vim/vim.nxe\nset_inode_field /apps/vim/vim.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"; \
	  printf "rm /bin/vim.nxe\n" | debugfs -w "$(IMAGE64_GRUB2_PART)" 2>/dev/null; \
	  printf "symlink /bin/vim.nxe /apps/vim/vim.nxe\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"; \
	  printf "mkdir /apps/vim/runtime\n" | debugfs -w "$(IMAGE64_GRUB2_PART)" 2>/dev/null; \
	  printf "rm /apps/vim/runtime/defaults.vim\nwrite user/vim-runtime/defaults.vim /apps/vim/runtime/defaults.vim\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"; \
	  printf "mkdir /nanos/share\nmkdir /nanos/share/terminfo\nmkdir /nanos/share/terminfo/x\n" | debugfs -w "$(IMAGE64_GRUB2_PART)" 2>/dev/null; \
	  infocmp xterm-256color 2>/dev/null \
	    | sed -E 's@setaf=[^,]*,@setaf=\\E[3%p1%dm,@; s@setab=[^,]*,@setab=\\E[4%p1%dm,@' \
	    > /tmp/xterm-256color.ti; \
	  tic -x -o /tmp/nanos-terminfo /tmp/xterm-256color.ti 2>/dev/null; \
	  printf "rm /nanos/share/terminfo/x/xterm-256color\nwrite /tmp/nanos-terminfo/x/xterm-256color /nanos/share/terminfo/x/xterm-256color\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"; \
	fi
	# terminfo DB for vim (mirror i686 _image): the xterm-256color entry, with setaf/setab rewritten
	# to the SIMPLE \E[3%p1%dm / \E[4%p1%dm form vim's term_color() drives correctly (the stock
	# conditional form leaks junk through vim's minimal tgoto). Shipped under /nanos/share/terminfo,
	# matching TERM=xterm-256color + TERMINFO. Done above, inside the vim block (its only consumer).
	# Doom (optional, in-tree doomgeneric): an /apps/doom bundle (the binary + its shareware
	# IWAD doom1.wad) + a /bin/doom.nxe symlink, installed only if `make ARCH=x86_64 doom`
	# staged bin/doom.nxe. Mirrors the i686 _image doom bundle (APP_PROGS + the WAD write), but
	# guarded by file presence since doom is not in the minimal X64_USER_PROGS subset. doom mmaps
	# /dev/fb0 and reads /dev/input0, both now present on x86_64 (the framebuffer multiboot tag).
	if [ -f $(BINFOLDER)doom.nxe ]; then \
	  printf "mkdir /apps/doom\n" | debugfs -w "$(IMAGE64_GRUB2_PART)" 2>/dev/null; \
	  printf "rm /apps/doom/doom.nxe\nwrite $(BINFOLDER)doom.nxe /apps/doom/doom.nxe\nset_inode_field /apps/doom/doom.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"; \
	  printf "rm /apps/doom/doom1.wad\nwrite disk/doom1.wad /apps/doom/doom1.wad\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"; \
	  printf "rm /bin/doom.nxe\n" | debugfs -w "$(IMAGE64_GRUB2_PART)" 2>/dev/null; \
	  printf "symlink /bin/doom.nxe /apps/doom/doom.nxe\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"; \
	fi
	# NetSurf graphical browser (optional, external): an /apps/netsurf bundle — the .nxe plus its
	# res/ tree (default/quirks/internal CSS, the Messages catalogue, the internal bitmap font,
	# icons, locale dirs) — plus a /bin/netsurf.nxe symlink (the app-bundle + link-farm pattern).
	# res/ is installed recursively (dirs first top-down, then files). Built by `make ARCH=x86_64
	# netsurf` (the netsurf-nanos port stack); launch inside nanowm. Skipped if bin/netsurf.nxe absent.
	# Mirrors the i686 _image netsurf population.
	if [ -f $(BINFOLDER)netsurf.nxe ]; then \
	  printf "mkdir /apps/netsurf\n" | debugfs -w "$(IMAGE64_GRUB2_PART)" 2>/dev/null; \
	  printf "rm /apps/netsurf/netsurf.nxe\nwrite $(BINFOLDER)netsurf.nxe /apps/netsurf/netsurf.nxe\nset_inode_field /apps/netsurf/netsurf.nxe mode 0100755\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"; \
	  printf "rm /bin/netsurf.nxe\n" | debugfs -w "$(IMAGE64_GRUB2_PART)" 2>/dev/null; \
	  printf "symlink /bin/netsurf.nxe /apps/netsurf/netsurf.nxe\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"; \
	  if [ -d $(BINFOLDER)netsurf-res ]; then \
	    printf "mkdir /apps/netsurf/res\n" | debugfs -w "$(IMAGE64_GRUB2_PART)" 2>/dev/null; \
	    ( cd $(BINFOLDER)netsurf-res && find . -mindepth 1 -type d | sed 's#^\./##' ) | while read d; do \
	      printf "mkdir /apps/netsurf/res/$$d\n" | debugfs -w "$(IMAGE64_GRUB2_PART)" 2>/dev/null; \
	    done; \
	    ( cd $(BINFOLDER)netsurf-res && find . -type f | sed 's#^\./##' ) | while read f; do \
	      printf "rm /apps/netsurf/res/$$f\nwrite $(BINFOLDER)netsurf-res/$$f /apps/netsurf/res/$$f\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"; \
	    done; \
	  fi; \
	fi
	# Reconcile the ext bitmaps after the debugfs writes so the built image is e2fsck-clean
	# (exit 1 = "fixed" is expected here, so don't fail the build on it).
	e2fsck -fy "$(IMAGE64_GRUB2_PART)" || true
	@echo "x86_64 disk image ready: $(IMAGE64_GRUB2)  (boot: $(QEMU) $(QEMU_CPU) $(QEMU_MEM) -drive file=$(IMAGE64_GRUB2),format=raw)"

-include $(OBJECTS:.o=.d)

# Build the GRUB2 ext2 skeleton once, then (re)write the kernel into it.
_grub2-image:
	./scripts/create-grub2-image.sh

_image: _all _userland _kext _grub2-image
	# System volume layout: NanOS itself lives under /nanos (core/bin/lib/kext/config/
	# cache/logs); non-system user apps live in /apps. GRUB stays in /boot. mkdir is
	# idempotent across rebuilds.
	-printf "mkdir /nanos\nmkdir /nanos/core\nmkdir /nanos/bin\nmkdir /nanos/lib\nmkdir /nanos/kext\nmkdir /nanos/config\nmkdir /nanos/cache\nmkdir /nanos/logs\nmkdir /nanos/share\nmkdir /nanos/share/terminfo\nmkdir /nanos/share/terminfo/x\nmkdir /apps\nmkdir /bin\n" | debugfs -w "$(IMAGE_GRUB2_PART)" 2>/dev/null
	# Kernel + init (PID 1) in core.
	printf "rm /nanos/core/kernel.bin\nwrite $(BINFOLDER)kernel.bin /nanos/core/kernel.bin\n" | debugfs -w "$(IMAGE_GRUB2_PART)"
	printf "rm /nanos/core/init.nxe\nwrite $(BINFOLDER)init.nxe /nanos/core/init.nxe\n" | debugfs -w "$(IMAGE_GRUB2_PART)"
	# Account database -> /nanos/config (NanOS keeps system config here, not in /etc). The
	# 7th field is the login shell: init/nterm launch getpwuid()->pw_shell, so editing this
	# file sets the default shell (the read-only-disk equivalent of chsh).
	printf "rm /nanos/config/passwd\nwrite config/passwd /nanos/config/passwd\n" | debugfs -w "$(IMAGE_GRUB2_PART)"
	# Network config templates -> /nanos/config/etc (copied into the writable /etc tmpfs at boot).
	-printf "mkdir /nanos/config/etc\n" | debugfs -w "$(IMAGE_GRUB2_PART)" 2>/dev/null
	for f in resolv.conf hosts nsswitch.conf protocols services inetd.conf shells; do \
	  printf "rm /nanos/config/etc/$$f\nwrite config/etc/$$f /nanos/config/etc/$$f\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
	done
	# DHCP: the udhcpc action helper (compiled .nxe; udhcpc exec()s it) -> /nanos/config/udhcpc.script,
	# plus the busybox udhcpc client itself -> /nanos/bin (only if `make udhcpc` staged it).
	printf "rm /nanos/config/udhcpc.script\nwrite $(BINFOLDER)dhcpcfg.nxe /nanos/config/udhcpc.script\n" | debugfs -w "$(IMAGE_GRUB2_PART)"
	if [ -f $(BINFOLDER)udhcpc.nxe ]; then \
	  printf "rm /nanos/bin/udhcpc.nxe\nwrite $(BINFOLDER)udhcpc.nxe /nanos/bin/udhcpc.nxe\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; fi
	# GNU git (optional, external): installed ONLY if `make git` staged bin/git.nxe. The single
	# binary goes to TWO places: /nanos/bin/git.nxe (the shell runs `git` -> .nxe by name) AND
	# /nanos/libexec/git-core/git (no extension) — git's compiled exec-path, where run-command
	# self-execs the literal program "git" for forked subcommands (git gc -> git pack-objects).
	if [ -f $(BINFOLDER)git.nxe ]; then \
	  printf "rm /nanos/bin/git.nxe\nwrite $(BINFOLDER)git.nxe /nanos/bin/git.nxe\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
	  printf "mkdir /nanos/libexec\nmkdir /nanos/libexec/git-core\n" | debugfs -w "$(IMAGE_GRUB2_PART)" 2>/dev/null; \
	  printf "rm /nanos/libexec/git-core/git.nxe\nwrite $(BINFOLDER)git.nxe /nanos/libexec/git-core/git.nxe\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
	  for c in gc repack pack-objects pack-refs prune prune-packed reflog rerere worktree maintenance commit-graph multi-pack-index fsck update-server-info upload-pack receive-pack; do \
	    printf "rm /nanos/libexec/git-core/git-%s.nxe\nln /nanos/libexec/git-core/git.nxe /nanos/libexec/git-core/git-%s.nxe\n" "$$c" "$$c" | debugfs -w "$(IMAGE_GRUB2_PART)" 2>/dev/null; \
	  done; \
	  echo "  installed git -> /nanos/bin/git.nxe + /nanos/libexec/git-core/{git,git-<cmd>}.nxe (run-command execs git-<cmd>; libc execve appends .nxe)"; fi
	# System utilities -> /nanos/bin.
	for p in $(SYS_PROGS); do \
	  printf "rm /nanos/bin/$$p.nxe\nwrite $(BINFOLDER)$$p.nxe /nanos/bin/$$p.nxe\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
	done
	# Non-system apps -> /apps. Each app is a self-contained BUNDLE directory
	# /apps/<name>/ holding <name>.nxe (the entry binary) plus any data files. /bin is a
	# flat link farm: a symbolic link /bin/<name>.nxe -> the app's bundle binary, so the
	# shell can run an app by name without knowing its bundle layout (à la /usr/local/bin).
	for p in $(APP_PROGS); do \
	  printf "mkdir /apps/$$p\n" | debugfs -w "$(IMAGE_GRUB2_PART)" 2>/dev/null; \
	  printf "rm /apps/$$p/$$p.nxe\nwrite $(BINFOLDER)$$p.nxe /apps/$$p/$$p.nxe\nset_inode_field /apps/$$p/$$p.nxe mode 0100755\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
	  printf "rm /bin/$$p.nxe\n" | debugfs -w "$(IMAGE_GRUB2_PART)" 2>/dev/null; \
	  printf "symlink /bin/$$p.nxe /apps/$$p/$$p.nxe\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
	done
	# Shared libraries the dynamic loader resolves against (see kernel/DynLoader.cpp).
	for l in $(USER_LIBS_NDL); do \
	  printf "rm /nanos/lib/$$l\nwrite $(BINFOLDER)$$l /nanos/lib/$$l\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
	done
	# Loadable kernel modules (.nkext) -> /nanos/kext; the kernel scans + loads them at boot.
	for m in $(KEXTS); do \
	  printf "rm /nanos/kext/$$m.nkext\nwrite $(BINFOLDER)$$m.nkext /nanos/kext/$$m.nkext\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
	done
	# GNU bash (optional): installed as an /apps/bash bundle + /bin link ONLY if `make bash`
	# staged bin/bash.nxe from the external fork. Skipped silently otherwise.
	if [ -f $(BINFOLDER)bash.nxe ]; then \
	  printf "mkdir /apps/bash\n" | debugfs -w "$(IMAGE_GRUB2_PART)" 2>/dev/null; \
	  printf "rm /apps/bash/bash.nxe\nwrite $(BINFOLDER)bash.nxe /apps/bash/bash.nxe\nset_inode_field /apps/bash/bash.nxe mode 0100755\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
	  printf "rm /bin/bash.nxe\n" | debugfs -w "$(IMAGE_GRUB2_PART)" 2>/dev/null; \
	  printf "symlink /bin/bash.nxe /apps/bash/bash.nxe\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
	fi
	# vim (optional, external): built by the nanos-sdk port and staged into bin/vim.nxe, same
	# pattern as bash. Its runtime (syntax/help/etc.) is a `data` entry the port driver installs
	# into the bundle; vim itself runs without it (`-u NONE`). Skipped if bin/vim.nxe is absent.
	if [ -f $(BINFOLDER)vim.nxe ]; then \
	  printf "mkdir /apps/vim\n" | debugfs -w "$(IMAGE_GRUB2_PART)" 2>/dev/null; \
	  printf "rm /apps/vim/vim.nxe\nwrite $(BINFOLDER)vim.nxe /apps/vim/vim.nxe\nset_inode_field /apps/vim/vim.nxe mode 0100755\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
	  printf "rm /bin/vim.nxe\n" | debugfs -w "$(IMAGE_GRUB2_PART)" 2>/dev/null; \
	  printf "symlink /bin/vim.nxe /apps/vim/vim.nxe\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
	  printf "mkdir /apps/vim/runtime\n" | debugfs -w "$(IMAGE_GRUB2_PART)" 2>/dev/null; \
	  printf "rm /apps/vim/runtime/defaults.vim\nwrite user/vim-runtime/defaults.vim /apps/vim/runtime/defaults.vim\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
	fi
	# bzip2 (optional, external): built by the nanos-sdk port and staged into bin/bzip2.nxe. A
	# system utility (flat in /nanos/bin) since it is a single self-contained binary. Skipped if
	# absent. bzip2 -d decompresses (same binary), so no separate bunzip2 is shipped.
	if [ -f $(BINFOLDER)bzip2.nxe ]; then \
	  printf "rm /nanos/bin/bzip2.nxe\nwrite $(BINFOLDER)bzip2.nxe /nanos/bin/bzip2.nxe\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
	fi
	# grep (optional, external): GNU grep built by the nanos-sdk port and staged into bin/grep.nxe.
	# A system utility (flat in /nanos/bin) — a single self-contained binary. Skipped if absent.
	if [ -f $(BINFOLDER)grep.nxe ]; then \
	  printf "rm /nanos/bin/grep.nxe\nwrite $(BINFOLDER)grep.nxe /nanos/bin/grep.nxe\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
	fi
	# ping (optional, external): GNU inetutils ping built by `make ping` (the nanos-sdk port) and
	# staged into bin/ping.nxe. A system utility (flat in /nanos/bin). Skipped if absent.
	if [ -f $(BINFOLDER)ping.nxe ]; then \
	  printf "rm /nanos/bin/ping.nxe\nwrite $(BINFOLDER)ping.nxe /nanos/bin/ping.nxe\nset_inode_field /nanos/bin/ping.nxe mode 0100755\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
	fi
	# wget (optional, external): GNU wget built by `make wget` (the nanos-sdk port), staged into
	# bin/wget.nxe. A system utility (flat in /nanos/bin). Skipped if absent.
	if [ -f $(BINFOLDER)wget.nxe ]; then \
	  printf "rm /nanos/bin/wget.nxe\nwrite $(BINFOLDER)wget.nxe /nanos/bin/wget.nxe\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
	fi
	# openssl (optional, external): OpenSSL CLI built by `make openssl` (the nanos-sdk port), staged
	# into bin/openssl.nxe. A system utility (flat in /nanos/bin). Skipped if absent.
	if [ -f $(BINFOLDER)openssl.nxe ]; then \
	  printf "rm /nanos/bin/openssl.nxe\nwrite $(BINFOLDER)openssl.nxe /nanos/bin/openssl.nxe\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
	fi
	# CA trust store (Mozilla bundle): the TLS clients (openssl s_client, wget https) verify server
	# certificate chains against /nanos/ssl/cert.pem = OpenSSL's compiled OPENSSLDIR. Shipped so a
	# guest TLS connection can return "Verify return code: 0 (ok)" without a per-command -CAfile.
	if [ -f disk-content/ssl/cert.pem ]; then \
	  printf "mkdir /nanos/ssl\n" | debugfs -w "$(IMAGE_GRUB2_PART)" 2>/dev/null; \
	  printf "rm /nanos/ssl/cert.pem\nwrite disk-content/ssl/cert.pem /nanos/ssl/cert.pem\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
	  printf "rm /nanos/ssl/openssl.cnf\nwrite disk-content/ssl/openssl.cnf /nanos/ssl/openssl.cnf\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
	fi
	# NetSurf graphical browser (optional, external): an /apps/netsurf bundle — the .nxe plus its
	# res/ tree (default/quirks/internal CSS, the Messages catalogue, the internal bitmap font,
	# icons, locale dirs) — plus a /bin/netsurf.nxe symlink (the app-bundle + link-farm pattern).
	# res/ is installed recursively (dirs first top-down, then files) so arbitrary nesting works.
	# Launch inside nanowm with `-f nanwm` (selects the nanowm libnsfb surface). Skipped if absent.
	if [ -f $(BINFOLDER)netsurf.nxe ]; then \
	  printf "mkdir /apps/netsurf\n" | debugfs -w "$(IMAGE_GRUB2_PART)" 2>/dev/null; \
	  printf "rm /apps/netsurf/netsurf.nxe\nwrite $(BINFOLDER)netsurf.nxe /apps/netsurf/netsurf.nxe\nset_inode_field /apps/netsurf/netsurf.nxe mode 0100755\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
	  printf "rm /bin/netsurf.nxe\n" | debugfs -w "$(IMAGE_GRUB2_PART)" 2>/dev/null; \
	  printf "symlink /bin/netsurf.nxe /apps/netsurf/netsurf.nxe\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
	  if [ -d $(BINFOLDER)netsurf-res ]; then \
	    printf "mkdir /apps/netsurf/res\n" | debugfs -w "$(IMAGE_GRUB2_PART)" 2>/dev/null; \
	    ( cd $(BINFOLDER)netsurf-res && find . -mindepth 1 -type d | sed 's#^\./##' ) | while read d; do \
	      printf "mkdir /apps/netsurf/res/$$d\n" | debugfs -w "$(IMAGE_GRUB2_PART)" 2>/dev/null; \
	    done; \
	    ( cd $(BINFOLDER)netsurf-res && find . -type f | sed 's#^\./##' ) | while read f; do \
	      printf "rm /apps/netsurf/res/$$f\nwrite $(BINFOLDER)netsurf-res/$$f /apps/netsurf/res/$$f\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
	    done; \
	  fi; \
	fi
	# Dropbear SSH (optional, external): server + keygen + client -> /nanos/bin. Also create root's
	# home (/disks/main/root, set in passwd) + a .ssh dir + /etc/dropbear (runtime host keys), so
	# pubkey auth has somewhere to read authorized_keys from and dropbear can store host keys.
	if [ -f $(BINFOLDER)dropbear.nxe ]; then \
	  for b in dropbear dropbearkey dbclient; do \
	    test -f $(BINFOLDER)$$b.nxe && printf "rm /nanos/bin/$$b.nxe\nwrite $(BINFOLDER)$$b.nxe /nanos/bin/$$b.nxe\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
	  done; \
	  printf "mkdir /root\nmkdir /root/.ssh\n" | debugfs -w "$(IMAGE_GRUB2_PART)" 2>/dev/null; \
	fi
	# inetd (optional, external): GNU inetutils inetd built by `make inetd` (the nanos-sdk services
	# port), staged into bin/inetd.nxe. A system utility (flat in /nanos/bin). Skipped if absent.
	if [ -f $(BINFOLDER)inetd.nxe ]; then \
	  printf "rm /nanos/bin/inetd.nxe\nwrite $(BINFOLDER)inetd.nxe /nanos/bin/inetd.nxe\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
	fi
	# telnetd (optional, external): GNU inetutils telnetd from the same services build. Remote bash
	# login over a kernel pty (launched by inetd; execs nanologin -> the user's shell).
	if [ -f $(BINFOLDER)telnetd.nxe ]; then \
	  printf "rm /nanos/bin/telnetd.nxe\nwrite $(BINFOLDER)telnetd.nxe /nanos/bin/telnetd.nxe\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
	fi
	# inetutils diagnostic clients (FAZA I): telnet / ifconfig / traceroute -> /nanos/bin.
	for b in telnet ifconfig traceroute; do \
	  if [ -f $(BINFOLDER)$$b.nxe ]; then \
	    printf "rm /nanos/bin/$$b.nxe\nwrite $(BINFOLDER)$$b.nxe /nanos/bin/$$b.nxe\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
	  fi; \
	done
	# darkhttpd (optional, external): single-file HTTP server -> /nanos/bin, plus its document
	# root /apps/www (the served site: index.html). Skipped if the binary is absent.
	if [ -f $(BINFOLDER)darkhttpd.nxe ]; then \
	  printf "rm /nanos/bin/darkhttpd.nxe\nwrite $(BINFOLDER)darkhttpd.nxe /nanos/bin/darkhttpd.nxe\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
	  printf "mkdir /apps/www\n" | debugfs -w "$(IMAGE_GRUB2_PART)" 2>/dev/null; \
	  printf "rm /apps/www/index.html\nwrite disk-content/www/index.html /apps/www/index.html\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
	fi
	# Desktop artwork. The wallpaper ships as the source PNG; nanowm decodes it and cover-fits it to
	# the live resolution at runtime. The logo stays a fixed 96x96 raw (About blits it directly).
	if [ -f assets/wallpaper.png ]; then \
	  printf "rm /nanos/share/wallpaper.png\nwrite assets/wallpaper.png /nanos/share/wallpaper.png\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
	fi
	if [ -f $(BINFOLDER)logo.raw ]; then \
	  printf "rm /nanos/share/logo.raw\nwrite $(BINFOLDER)logo.raw /nanos/share/logo.raw\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
	fi
	# Doom's shareware IWAD is a data file inside the doom app bundle (its layer -iwad's it).
	printf "rm /apps/doom/doom1.wad\nwrite disk/doom1.wad /apps/doom/doom1.wad\n" | debugfs -w "$(IMAGE_GRUB2_PART)"
	# terminfo database: the xterm-256color entry (matches TERM), shipped under /nanos/share/
	# terminfo. We rewrite setaf/setab to the SIMPLE \E[3%p1%dm / \E[4%p1%dm form instead of the
	# stock conditional `%?%p1%{8}%<%t...` string. Reason: vim's term_color() recognises the
	# simple form and does its OWN 16-vs-256-colour selection (emitting \E[3Nm / \E[9Nm / \E[38;5;Nm
	# correctly), whereas for any other setaf it falls back to its minimal tgoto(), which does not
	# implement terminfo's %{n}/%<%/%t conditional+arithmetic grammar and leaks junk like
	# "6}38;5;Nm". Recompiled with tic. (Properly fixing the general case means full terminfo
	# param support in vim's tgoto — out of scope; this is the standard "give vim a terminfo it
	# can drive" approach used by other minimal systems.)
	infocmp xterm-256color 2>/dev/null \
	  | sed -E 's@setaf=[^,]*,@setaf=\\E[3%p1%dm,@; s@setab=[^,]*,@setab=\\E[4%p1%dm,@' \
	  > /tmp/xterm-256color.ti
	tic -x -o /tmp/nanos-terminfo /tmp/xterm-256color.ti 2>/dev/null
	printf "rm /nanos/share/terminfo/x/xterm-256color\nwrite /tmp/nanos-terminfo/x/xterm-256color /nanos/share/terminfo/x/xterm-256color\n" | debugfs -w "$(IMAGE_GRUB2_PART)"
	# Reconcile the ext block/inode bitmaps after the debugfs writes: `debugfs write` can leave the
	# free-counts/bitmaps slightly off, so a final `e2fsck -fy` makes every built image e2fsck-clean
	# (exit 1 = "fixed", which is expected here, so don't fail the build on it).
	e2fsck -fy "$(IMAGE_GRUB2_PART)" || true

_iso: _all
	mkdir -p iso/boot/grub
	cp $(BINFOLDER)kernel.bin iso/boot/
	cp grub.cfg iso/boot/grub/
	grub-mkrescue -o nanos.iso iso/

_clean:
	-rm $(BINFOLDER)*.o $(BINFOLDER)*.d $(BINFOLDER)kernel.bin
	-rm $(BINFOLDER)*.elf $(BINFOLDER)*.nxe $(BINFOLDER)*.ndl $(BINFOLDER)*.a $(BINFOLDER)mknx
	-rm -rf $(BINFOLDER)libimp $(BINFOLDER)k64

# ----------------------------------------------------------------------------
# Userland: .nxe programs link against ported picolibc + our syscall glue (own
# linker script/base 0x400000, NOT in kernel SOURCES). picolibc headers come via
# -isystem; SyscallNr.h via -Ikernel. No <string.h> clash (lib/ is not on the path).
# ----------------------------------------------------------------------------
# picolibc + the userland codegen flags are arch-selected: i686 uses the 32-bit sysroot and
# the plain (no extra arch) flags; x86_64 uses the 64-bit sysroot built into nanos-build
# (commit 170f150) and the SysV-AMD64 userland flags (SSE on for varargs/float, small code
# model + non-PIC for the fixed low base, no red zone). i686 is unchanged below.
ifeq ($(ARCH),x86_64)
PICOLIBC=/opt/picolibc/x86_64-elf
USER_ARCHFLAGS=-mcmodel=small -mno-red-zone
else
PICOLIBC=/opt/picolibc/i686-elf
USER_ARCHFLAGS=
endif
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
# Programs built. Placement (see _image): init -> /nanos/core (PID 1); system utilities
# -> /nanos/bin; non-system apps (games/demos/tests) -> /apps.
USER_PROGS=init nsh cat ls mkdir rmdir pwd touch rm ln cp mv chmod wc head tail true false env basename dirname sigtest fbtest timetest brktest inputtest fstest free usedll pipetest forkmany orphan clonetest ptytest nterm tuitest racetest envtest mmaptest mousetest doom nwm notepad form rustform nwexp settings terminal about crashtest socktest pingtest nettest unixtest tcpsrv nanologin greeter dhcpcfg randhex errnotest pthrtest pthrstress pfract smptorture nettorture
SYS_PROGS=nsh cat ls mkdir rmdir pwd touch rm ln cp mv chmod wc head tail true false env basename dirname free nwm greeter socktest pingtest nettest unixtest tcpsrv nanologin randhex errnotest
APP_PROGS=sigtest fbtest timetest brktest inputtest fstest usedll pipetest forkmany orphan clonetest ptytest nterm tuitest racetest envtest mmaptest mousetest doom notepad form rustform nwexp settings terminal about crashtest pthrtest pthrstress pfract smptorture nettorture
# Shared libraries (.ndl) shipped to /nanos/lib (see _image).
USER_LIBS_NDL=greet.ndl libc.ndl libnw.ndl libnwui.ndl
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
# GNU-as (AT&T) — i686-elf-gcc assembles them directly.
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

# mknx: host build tool (native cc) that turns the linked ELF into a .nxe/.ndl —
# extracts the load image + R_386_32 base relocations + exports/imports, replacing
# `objcopy -O binary`. Needs the format header (kernel/NxFormat.h) via -Ikernel.
MKNX=$(BINFOLDER)mknx
$(MKNX): tools/mknx.c kernel/NxFormat.h
	@mkdir -p $(BINFOLDER)
	cc -O2 -Wall -Ikernel -o $@ tools/mknx.c

# 64-bit variant: same source, -DNX_FORCE64 selects the ELF64 / R_X86_64_* path and the
# v4 .nxe layout (64-bit nxaddr_t). Host-native cc, just like mknx.
MKNX64=$(BINFOLDER)mknx64
$(MKNX64): tools/mknx.c kernel/NxFormat.h
	@mkdir -p $(BINFOLDER)
	cc -O2 -Wall -DNX_FORCE64 -Ikernel -o $@ tools/mknx.c

# Arch-selected mknx for the libc.ndl chain: i686 emits the 32-bit (R_386_32) format, x86_64
# the ELF64 (R_X86_64_*) v4 layout. The shared-library rules below reference $(MKNX_TOOL) so
# they produce the right format for the active ARCH.
ifeq ($(ARCH),x86_64)
MKNX_TOOL=$(MKNX64)
# Userland linker script: x86_64 programs use the elf64-x86-64 variant (OUTPUT_FORMAT +
# the low fixed base for R_X86_64_32S); i686 uses the original 32-bit script.
USER_NX_LD=arch/x86_64/user-nx.ld
else
MKNX_TOOL=$(MKNX)
USER_NX_LD=user/nx.ld
endif

ifeq ($(ARCH),x86_64)
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
# the main-thread TLS bootstrap (calls __nx_init_tls before main, see user/crt064.S) and must
# be assembled from the 64-bit startup; an explicit rule overrides the generic user/%.S
# pattern (which would wrongly assemble the i686 user/crt0.S as elf64). nxhdr.o builds from
# user/nxhdr.c via the generic user/%.c rule (USER_CFLAGS is now the x86_64 picolibc set), so
# it needs no override here.
$(BINFOLDER)crt0.o: user/crt064.S
	@mkdir -p $(BINFOLDER)
	nasm -f elf64 $< -o $@

# The signal trampoline is arch-specific: bin/sigtramp.o (in LIBC_GLUE_OBJS) is the 64-bit
# __nx_sigtramp from user/sigtramp64.S, overriding the generic user/%.S pattern (which would
# wrongly take the [BITS 32] user/sigtramp.S). Same exported symbol (__nx_sigtramp).
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
endif

# Generic DYNAMIC link: every program links its objects + the import library (NO static
# libc) and declares "needed: libc.ndl", so the loader maps libc.ndl and binds its imports
# by name. `--emit-relocs` keeps the R_386_32 relocations so mknx can build the relocation
# table (the .nxe loads at any base). Each program below just declares its object prereqs.
$(BINFOLDER)%.nxe: $(MKNX_TOOL)
	$(LD) -nostdlib -Wl,--emit-relocs -T $(USER_NX_LD) -o $(@:.nxe=.elf) $(filter %.o,$^) $(filter %.a,$^) -lgcc
	$(MKNX_TOOL) $(@:.nxe=.elf) $@ --need libc.ndl

# Per-program object sets: DYN_GLUE (crt0+nxhdr) + program objects + the libc import
# library (an ARCHIVE — the linker pulls only the members the program references, so it
# imports just the symbols it uses). libc.ndl is a prereq so it is built/shipped. Doom +
# usedll have explicit rules (extra math / a second needed library).
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
$(BINFOLDER)socktest.nxe:  $(DYN_DEPS) $(BINFOLDER)socktest.o
$(BINFOLDER)pingtest.nxe:  $(DYN_DEPS) $(BINFOLDER)pingtest.o
$(BINFOLDER)nettest.nxe:   $(DYN_DEPS) $(BINFOLDER)nettest.o
$(BINFOLDER)unixtest.nxe:  $(DYN_DEPS) $(BINFOLDER)unixtest.o
$(BINFOLDER)tcpsrv.nxe:    $(DYN_DEPS) $(BINFOLDER)tcpsrv.o
$(BINFOLDER)drmtest.nxe:   $(DYN_DEPS) $(BINFOLDER)drmtest.o
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
$(BINFOLDER)pthrtest.nxe: $(DYN_DEPS) $(BINFOLDER)pthrtest.o
$(BINFOLDER)pthrstress.nxe: $(DYN_DEPS) $(BINFOLDER)pthrstress.o
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
# rustform: the SAME demo written in RUST, proving the C-ABI libnwui is language-agnostic. A
# cargo staticlib (no_std, -Z build-std for the bare i686-nanos target) is linked with crt0 +
# the import libraries, then mknx'd like any app; --need libnwui.ndl pulls the whole chain.
RUST_TARGET=user/rust/i686-nanos.json
RUST_LIB=user/rust/rustform/target/i686-nanos/release/librustform.a
$(RUST_LIB): user/rust/rustform/src/lib.rs user/rust/rustform/src/nanos.rs user/rust/rustform/Cargo.toml $(RUST_TARGET)
	cd user/rust/rustform && cargo build -Z build-std=core,alloc -Z json-target-spec --target ../i686-nanos.json --release
$(BINFOLDER)rustform.nxe: $(DYN_GLUE) $(RUST_LIB) $(BINFOLDER)libnwui.ndl.a $(BINFOLDER)libc.ndl.a $(BINFOLDER)libnwui.ndl $(BINFOLDER)libnw.ndl $(BINFOLDER)libc.ndl $(MKNX)
	$(LD) -nostdlib -Wl,--emit-relocs -T user/nx.ld -o $(BINFOLDER)rustform.elf $(DYN_GLUE) $(RUST_LIB) $(BINFOLDER)libnwui.ndl.a $(BINFOLDER)libc.ndl.a -lgcc
	$(MKNX) $(BINFOLDER)rustform.elf $@ --need libnwui.ndl
# rsexp: the x86_64 Rust file explorer (the desktop's Files app). Same scheme as rustform but for
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

# ---- Stage-2 dynamic-linking demo: greet.ndl (shared lib) + usedll (imports from it) ----
$(BINFOLDER)greet.o: user/lib/greet.c
	@mkdir -p $(BINFOLDER)
	$(CXX) $(USER_CFLAGS) -MMD -MP -c $< -o $@
$(BINFOLDER)greet_import.o: user/lib/greet_import.S
	@mkdir -p $(BINFOLDER)
	nasm -f elf $< -o $@

# greet.ndl: a relocatable shared library exporting nx_greet/nx_greeting. No crt0/libc —
# just the header placeholder (nxhdr.o) + the library code. mknx --dll marks it a library
# and emits the named export table the loader binds against.
$(BINFOLDER)greet.ndl: $(BINFOLDER)nxhdr.o $(BINFOLDER)greet.o $(MKNX)
	$(LD) -nostdlib -Wl,--emit-relocs -T user/dll.ld -o $(BINFOLDER)greet.elf $(BINFOLDER)nxhdr.o $(BINFOLDER)greet.o
	$(MKNX) $(BINFOLDER)greet.elf $@ --dll --export nx_greet --export nx_greeting

# usedll: imports printf from libc.ndl AND nx_greet/nx_greeting from greet.ndl — two needed
# libraries, so it has its own rule with both --need flags.
$(BINFOLDER)usedll.nxe: $(DYN_GLUE) $(BINFOLDER)usedll.o $(BINFOLDER)greet_import.o $(BINFOLDER)libc.ndl.a $(BINFOLDER)libc.ndl $(BINFOLDER)greet.ndl $(MKNX)
	$(LD) -nostdlib -Wl,--emit-relocs -T user/nx.ld -o $(BINFOLDER)usedll.elf $(DYN_GLUE) $(BINFOLDER)usedll.o $(BINFOLDER)greet_import.o $(BINFOLDER)libc.ndl.a -lgcc
	$(MKNX) $(BINFOLDER)usedll.elf $@ --need libc.ndl --need greet.ndl

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
_userland: $(addprefix $(BINFOLDER),$(addsuffix .nxe,$(USER_PROGS))) $(addprefix $(BINFOLDER),$(USER_LIBS_NDL))

# x86_64 in-tree userland subset (Plan 10 Task 7): the shell + the sbase coreutils + init,
# all dynamically linked against the 64-bit libc.ndl (the same dynamic path as i686, via the
# arch-selected MKNX_TOOL/USER_NX_LD). The full USER_PROGS set (nanowm, Rust demo, Doom, the
# pthread/net stress tools) is NOT built here — those are later ports; this is the first
# interactive 64-bit milestone (a working shell + ls/cat). init goes to /nanos/core, the
# rest to /nanos/bin (see _image64). free is a system util like the coreutils.
X64_SYS_PROGS=nsh open nanosu cat ls mkdir rmdir pwd touch rm ln cp mv chmod wc head tail true false env basename dirname free chsh pfract pthrstress smptorture nettorture drmtest glpix malloctest
# nanowm compositor (nwm) is a system GUI program; the NetSurf libnsfb backend (and future GUI
# clients) link the libnw/libnwui import libs at load, so those .ndl ship to /nanos/lib too.
X64_GUI_PROGS=nwm greeter
# nanowm desktop client apps. nwm spawns them by absolute path from /disks/main/apps/<name>/<name>.nxe
# (see NWEXP_PATH etc. in user/nwm/nwm.c), so — unlike the compositor — they install as /apps bundles
# (+ a /bin symlink), exactly like the i686 APP_PROGS loop, NOT into /nanos/bin.
X64_GUI_APPS=rsexp settings about notepad viewer properties form terminal
X64_GUI_LIBS=libnw.ndl libnwui.ndl
X64_USER_PROGS=init $(X64_SYS_PROGS) $(X64_GUI_PROGS) $(X64_GUI_APPS)
_userland64: $(addprefix $(BINFOLDER),$(addsuffix .nxe,$(X64_USER_PROGS))) $(BINFOLDER)libc.ndl $(addprefix $(BINFOLDER),$(X64_GUI_LIBS))

# ----------------------------------------------------------------------------
# Kernel modules (nkext): loadable drivers built SEPARATELY from kernel.bin, shipped to
# /nanos/kext and loaded at boot (kernel/KextLoader). A kext is an NxFormat module (like a
# .ndl) but its imports resolve to the KERNEL export table (KernelExports), not libc.ndl. Kext
# code is ring-0 freestanding C++ (no picolibc); -Iinclude gives the freestanding string.h and
# we deliberately omit -Ilib so the macOS case-insensitivity trap can't bite when building in
# /src. (See docs/filesystem.md: /nanos/kext.)
# Kexts run in ring 0 with the KERNEL ABI, so they take the same machine-dependent codegen
# flags as the kernel ($(KARCHFLAGS): on x86_64 that is -mno-red-zone -mno-sse -mno-mmx
# -mno-80387 — a ring-0 module must not touch SSE/x87 or the red zone across interrupts;
# empty on i686). $(CXX)/$(LD) are already the arch cross toolchain ($(CROSS)gcc).
KEXT_CFLAGS=-ffreestanding -nostdlib -nostdinc++ --no-exceptions --no-rtti \
  -fno-sized-deallocation -fno-leading-underscore -fno-pic -fno-stack-protector \
  -Iarch/include -Ikernel -Idrivers -Iinclude $(KARCHFLAGS) $(KOPTFLAGS)

# Arch knobs for the kext format: x86_64 emits ELF64 (.nxlib.kernel slots are 8-byte `dq`,
# RIP-relative `jmp [rel ...]` thunks, base < 2 GiB via kext64.ld) and is packed by mknx64
# (v4 / R_X86_64_*); i686 keeps the historical 32-bit thunks (4-byte `dd`, absolute `jmp`)
# and mknx (v3 / R_386_32).
ifeq ($(ARCH),x86_64)
KEXT_LD=kext/kext64.ld
KIMP_BITS=64
KIMP_DW=dq
KIMP_ALIGN=8
KIMP_REL=rel
else
KEXT_LD=kext/kext.ld
KIMP_BITS=32
KIMP_DW=dd
KIMP_ALIGN=4
KIMP_REL=
endif

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
  $(BINFOLDER)kpi_sg.o $(BINFOLDER)kpi_fence.o $(BINFOLDER)kpi_misc.o
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

KEXTS=kbd mouse e1000 e1000e i219
# The LinuxKPI virtio_gpu module is x86_64-only (vendored Linux source assumes 64-bit).
ifeq ($(ARCH),x86_64)
KEXTS+= virtio_gpu
endif
_kext: $(addprefix $(BINFOLDER),$(addsuffix .nkext,$(KEXTS)))

# Doom (doomgeneric). Old-C source needs -fcommon (GCC 10+ defaults to -fno-common, which
# breaks Doom's tentative globals) and warnings off; -DNORMALUNIX -DLINUX select the POSIX
# code paths; -lm for the renderer's trig/sqrt. Our platform layer (doomgeneric_nanos.c)
# replaces the shipped backends. Built as the `doom` program in USER_PROGS.
DOOM_DIR=user/third_party/doomgeneric
DOOM_CFLAGS=-ffreestanding -isystem $(PICOLIBC)/include -iquote kernel -Iuser -I$(DOOM_DIR) -D_DEFAULT_SOURCE -DNORMALUNIX -DLINUX -include user/libc-glue/compat-decls.h -include user/libc-glue/nx-dllimport.h -w -fcommon -fno-pic -fno-stack-protector $(USER_ARCHFLAGS) $(UOPTFLAGS)
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

# Arch-aware `make doom`: stage bin/doom.nxe for the selected arch (i686 default, or
# ARCH=x86_64). The .nxe rule above already uses the arch-selected toolchain ($(CXX)/$(LD)),
# 64-bit picolibc sysroot ($(PICOLIBC)), linker script ($(USER_NX_LD)) and mknx ($(MKNX_TOOL)),
# so the SAME rule produces an ELF32 v3 .nxe for i686 or an ELF64 v4 .nxe for x86_64. On i686
# doom is also part of USER_PROGS (built by `make image`); on x86_64 it is NOT in the minimal
# X64_USER_PROGS subset, so `make ARCH=x86_64 doom` is the way to stage it, and `make image64`
# then installs the /apps/doom bundle + /bin/doom.nxe symlink (only if bin/doom.nxe exists).
.PHONY: doom
doom: $(BINFOLDER)doom.nxe
	@echo "staged $(BINFOLDER)doom.nxe — run 'make image' (i686) or 'make image64' (x86_64) to install the /apps/doom bundle"

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
# AddressSpace + its paging headers are arch-specific (2-level i686 vs 4-level x86_64).
# Host-test the variant matching ARCH: the include path picks the right Paging.h/AddressSpace.h,
# the matching .cpp is compiled, and the OTHER arch's tests are filtered out (their uint32_t vs
# uint64_t API would not compile against the swapped header). Default ARCH=x86 is unchanged.
ifeq ($(ARCH),x86_64)
ARCH_MM_INC=-Iarch/x86_64/mm
ARCH_ADDRSPACE=arch/x86_64/mm/AddressSpace.cpp
ARCH_PAGING_TESTS_EXCL=tests/test_addressspace.cpp tests/test_paging.cpp
else
ARCH_MM_INC=-Iarch/x86/mm
ARCH_ADDRSPACE=arch/x86/mm/AddressSpace.cpp
ARCH_PAGING_TESTS_EXCL=tests/test_addressspace64.cpp tests/test_paging64.cpp
endif
# Host include path: code dirs only, deliberately WITHOUT -Iinclude so that
# <string.h> resolves to libc (not the freestanding include/string.h).
HINCLUDES=-Iarch/include -Ikernel -Idrivers -Ifs -Imm -Ilib -Inet -Iusb -Iarch/x86/boot $(ARCH_MM_INC) -Iarch/x86_64/cpu -Ikext/mouse -Ikext/e1000 -Ikext/i219 -Ikext/virtio_gpu -Iuser/libnw -Iuser/nwm -Iuser/libnwui -Iuser/term -Iuser/third_party/stb -Iuser/libc-glue -Ilinuxkpi/include -Ilinuxkpi
# The host is LP64 (arm64/x86_64) but does not define __x86_64__, so force the v4 64-bit
# .nx format (nxaddr_t = uint64_t) across the whole host test build. This exercises the
# x86_64 loader path (R_X86_64_64 fixups, 8-byte IAT slots) and keeps every TU's view of
# NxFormat.h consistent (NxeLoader.cpp + its test must agree on the loadImage signature).
HOST_CXXFLAGS=-std=c++17 -O0 -g $(HINCLUDES) -Wall --coverage -DNX_FORCE64=1 -DNANOS_HOST_TEST=1
TEST_BIN=/tmp/nanos_tests
TEST_SRCS=$(filter-out $(ARCH_PAGING_TESTS_EXCL),$(wildcard tests/*.cpp))
# Modules under test (grown as layers are added). Header-only modules contribute
# coverage via the .h patterns below.
TEST_MODULES=drivers/RamBlockDevice.cpp drivers/DeviceManager.cpp drivers/Console.cpp fs/Vfs.cpp fs/ExtFilesystem.cpp fs/SynthFs.cpp fs/RamFs.cpp fs/ext/Crc32c.cpp fs/ext/BlockCache.cpp fs/ext/ExtCsum.cpp fs/ext/ExtAllocator.cpp fs/ext/Journal.cpp kernel/Syscall.cpp kernel/Cred.cpp kernel/NxeLoader.cpp kernel/KeyDecoder.cpp kernel/Scheduler.cpp kernel/Process.cpp kernel/Signal.cpp kernel/Futex.cpp kernel/Csprng.cpp kernel/Acpi.cpp kernel/Bkl.cpp lib/String.cpp
TEST_MODULES+= arch/x86/boot/MultibootMmap.cpp mm/FrameAllocator.cpp mm/Heap.cpp $(ARCH_ADDRSPACE)
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
TEST_MODULES+= linuxkpi/kpi_slab.c linuxkpi/kpi_print.c linuxkpi/kpi_idr.c linuxkpi/kpi_sort.c linuxkpi/kpi_time.c
# lcov patterns selecting the modules whose coverage is gated (String is support).
COV_PATTERNS="*/RamBlockDevice.*" "*/DeviceManager.*" "*/Vfs.*" "*/ExtFilesystem.*" "*/Ext2Filesystem.*" "*/Ext4Filesystem.*" "*/ExtAllocator.*" "*/BlockCache.*" "*/ExtCsum.*" "*/Crc32c.*" "*/Journal.*" "*/SynthFs.*" "*/RamFs.*" "*/Syscall.*" "*/Cred.*" "*/NxeLoader.*" "*/KeyDecoder.*" "*/Process.*" "*/Signal.*" "*/Futex.*" "*/Csprng.*" "*/Acpi.*" "*/Framebuffer.*" "*/FbConsole.*" "*/Fbdev.*" "*/KeyboardDevice.*" "*/Pty.*" "*/MouseDevice.*" "*/MultibootMmap.*" "*/FrameAllocator.*" "*/Heap.*" "*/AddressSpace.*" "*/nwproto.*" "*/nw_gfx.*" "*/nw_settings.*" "*/nwm_core.*" "*/nw_compose.*" "*/nw_backdrop.*" "*/nwui_core.*" "*/vt.*" "*/Pci.*" "*/MsiRouter.*" "*/lapic_x86_64.*" "*/e1000_core.*" "*/i219_phy.*" "*/Net.*" "*/NetBuf.*" "*/NetDevice.*" "*/Loopback.*" "*/Ether.*" "*/Arp.*" "*/Ip.*" "*/Route.*" "*/Icmp.*" "*/Socket.*" "*/Udp.*" "*/Raw.*" "*/Tcp.*" "*/Packet.*" "*/Unix.*" "*/NetProc.*" "*/NetStats.*" "*/resolv_parse.*" "*/crypt.*" "*/UsbCore.*" "*/UsbHid.*" "*/UsbMsc.*" "*/UsbMscBlockDevice.*" "*/PartitionTable.*" "*/GdtBase.*"
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

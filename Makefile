# Architecture selection: machine-dependent knobs come from arch/$(ARCH)/arch.mk
# (CROSS, ARCH_VPATH, ARCH_INCLUDES, ARCH_LINKER, ARCH_SOURCES).
ARCH ?= x86
include arch/$(ARCH)/arch.mk

# Machine-independent objects (portable across architectures).
MI_SOURCES=kmain.o Kernel.o Console.o ExtFilesystem.o SynthFs.o RamFs.o RamBlockDevice.o DeviceManager.o Vfs.o
MI_SOURCES+= Crc32c.o BlockCache.o ExtCsum.o ExtAllocator.o Journal.o
MI_SOURCES+= Framebuffer.o Font8x16.o FbConsole.o vtk.o Fbdev.o Fb0Device.o KeyboardDevice.o Pty.o
MI_SOURCES+= Syscall.o SyscallDispatch.o NxeLoader.o Exec.o DynLoader.o KernelExports.o KextLoader.o FrameAllocator.o KeyDecoder.o Scheduler.o Process.o Signal.o Futex.o Csprng.o
MI_SOURCES+= Pci.o
MI_SOURCES+= Net.o NetBuf.o NetDevice.o Loopback.o NetCore.o Ether.o Arp.o Ip.o Route.o Icmp.o
MI_SOURCES+= Socket.o Udp.o Raw.o Tcp.o Packet.o Unix.o NetProc.o NetStats.o
MI_SOURCES+= memory_manager.o Heap.o List.o String.o icxxabi.o string_funcs.o
# Full link set = portable objects + the selected arch's machine-dependent objects.
SOURCES=$(MI_SOURCES) $(ARCH_SOURCES)

BINFOLDER=bin/
IMAGE_GRUB2=disk/image-grub2.img
# Partition starts at LBA 2048 (1MiB offset)
IMAGE_GRUB2_PART=$(IMAGE_GRUB2)?offset=1048576

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
bash: docker-image
	@test -f "$(BASH_FORK)/nanos/build.sh" || { echo "bash fork not found at $(BASH_FORK)/nanos (set BASH_FORK=/path/to/bash-nanos)"; exit 1; }
	docker run --rm -v $(CURDIR):/src -v "$(BASH_FORK)":/bash -w /bash $(DOCKER_IMAGE) sh /bash/nanos/build.sh build
	cp "$(BASH_FORK)/nanos/bash.nxe" $(BINFOLDER)bash.nxe
	@echo "staged $(BINFOLDER)bash.nxe — run 'make image' to install it into /apps/bash"

# GNU grep (optional, external). Built by the nanos-sdk in its own work dir (cross toolchain +
# gnulib). This target only copies the finished grep.nxe into bin/, where _image installs it as
# the /nanos/bin/grep system utility. The SDK build itself lives outside this repo (see the
# nanos-sdk-and-vim-port notes); `make image` never depends on this, so a missing artifact can't
# break a normal build.
SDK_WORK ?= $(HOME)/Projects/nanos-sdk-work
grep:
	@test -f "$(SDK_WORK)/grep-3.11/src/grep.nxe" || { echo "grep.nxe not found at $(SDK_WORK)/grep-3.11/src (build it with the nanos-sdk first)"; exit 1; }
	cp "$(SDK_WORK)/grep-3.11/src/grep.nxe" $(BINFOLDER)grep.nxe
	@echo "staged $(BINFOLDER)grep.nxe — run 'make image' to install it into /nanos/bin"

# GNU vim + bzip2 (optional, external) — same as grep: the nanos-sdk builds them in its work dir;
# these targets only copy the finished .nxe into bin/, where _image installs them.
vim:
	@test -f "$(SDK_WORK)/vim/src/vim.nxe" || { echo "vim.nxe not found at $(SDK_WORK)/vim/src (build it with the nanos-sdk first)"; exit 1; }
	cp "$(SDK_WORK)/vim/src/vim.nxe" $(BINFOLDER)vim.nxe
	@echo "staged $(BINFOLDER)vim.nxe — run 'make image' to install it into /apps/vim"
bzip2:
	@test -f "$(SDK_WORK)/bzip2-1.0.8/bzip2.nxe" || { echo "bzip2.nxe not found at $(SDK_WORK)/bzip2-1.0.8 (build it with the nanos-sdk first)"; exit 1; }
	cp "$(SDK_WORK)/bzip2-1.0.8/bzip2.nxe" $(BINFOLDER)bzip2.nxe
	@echo "staged $(BINFOLDER)bzip2.nxe — run 'make image' to install it into /nanos/bin"

# GNU inetutils ping (optional, external). The plan's real ping: SOCK_RAW/ICMP + getaddrinfo,
# unmodified upstream. Unlike grep/vim (prebuilt by hand), this target is REPRODUCIBLE: it
# refreshes the SDK sysroot from THIS checkout (net/POSIX headers in user/libc-glue/include +
# the freshly built libc.ndl{,.a}) and drives nanos-port inside the nanos-sdk-dev container.
# The manifest (configure flags + cross-compile cache) lives at $(SDK_WORK)/inetutils-port.
# `make image` never depends on this; a missing toolchain/port errors clearly and is skipped.
NANOS_SDK   ?= $(HOME)/Projects/nanos-sdk
SDK_TC      := $(SDK_WORK)/toolchain
PING_PORT   := $(SDK_WORK)/inetutils-port
ping: bin/libc.ndl bin/libc.ndl.a
	@test -d "$(SDK_TC)/i686-nanos/include" || { echo "nanos-sdk toolchain not found at $(SDK_TC)"; exit 1; }
	@test -f "$(PING_PORT)/nxport.toml"     || { echo "inetutils port not found at $(PING_PORT)/nxport.toml"; exit 1; }
	# Refresh the SDK sysroot from this repo (the source of truth for headers + libc).
	cp -R user/libc-glue/include/. "$(SDK_TC)/i686-nanos/include/"
	cp kernel/SyscallNr.h          "$(SDK_TC)/i686-nanos/include/SyscallNr.h"
	cp $(BINFOLDER)libc.ndl.a      "$(SDK_TC)/i686-nanos/lib/libc.a"
	cp $(BINFOLDER)libc.ndl        "$(SDK_TC)/i686-nanos/lib/libc.ndl"
	docker run --rm \
	  -v "$(SDK_TC)":/work/toolchain -v "$(PING_PORT)":/work/port -v "$(NANOS_SDK)":/sdk \
	  -e SDK=/sdk -e PATH="/work/toolchain/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
	  -w /work/port nanos-sdk-dev:latest python3 /sdk/port/nanos-port /work/port
	cp "$(PING_PORT)/ping.nxe" $(BINFOLDER)ping.nxe
	@echo "staged $(BINFOLDER)ping.nxe — run 'make image' to install it into /nanos/bin"

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
# reproducible flow as ping/wget/httpd. `make image` never depends on this.
OPENSSL_PORT := $(SDK_WORK)/openssl-port
openssl: bin/libc.ndl bin/libc.ndl.a
	@test -d "$(SDK_TC)/i686-nanos/include" || { echo "nanos-sdk toolchain not found at $(SDK_TC)"; exit 1; }
	@test -f "$(OPENSSL_PORT)/nxport.toml"   || { echo "openssl port not found at $(OPENSSL_PORT)/nxport.toml"; exit 1; }
	cp -R user/libc-glue/include/. "$(SDK_TC)/i686-nanos/include/"
	cp kernel/SyscallNr.h          "$(SDK_TC)/i686-nanos/include/SyscallNr.h"
	cp $(BINFOLDER)libc.ndl.a      "$(SDK_TC)/i686-nanos/lib/libc.a"
	cp $(BINFOLDER)libc.ndl        "$(SDK_TC)/i686-nanos/lib/libc.ndl"
	docker run --rm \
	  -v "$(SDK_TC)":/work/toolchain -v "$(OPENSSL_PORT)":/work/port -v "$(NANOS_SDK)":/sdk \
	  -e SDK=/sdk -e PATH="/work/toolchain/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
	  -w /work/port nanos-sdk-dev:latest python3 /sdk/port/nanos-port /work/port
	cp "$(OPENSSL_PORT)/openssl.nxe" $(BINFOLDER)openssl.nxe
	@echo "staged $(BINFOLDER)openssl.nxe — run 'make image' to install it into /nanos/bin"

# Dropbear (optional, external): small SSH-2 server (dropbear) + keygen (dropbearkey) + client
# (dbclient) — FAZA 4/5 of the TLS/SSH plan. Bundles its own crypto, runs as root without privsep.
# Built by the nanos-sdk from $(DROPBEAR_PORT)/nxport.toml. Same reproducible flow. `make image`
# installs them; the host reaches sshd via hostfwd 2222->22.
DROPBEAR_PORT := $(SDK_WORK)/dropbear-port
dropbear: bin/libc.ndl bin/libc.ndl.a
	@test -d "$(SDK_TC)/i686-nanos/include" || { echo "nanos-sdk toolchain not found at $(SDK_TC)"; exit 1; }
	@test -f "$(DROPBEAR_PORT)/nxport.toml"   || { echo "dropbear port not found at $(DROPBEAR_PORT)/nxport.toml"; exit 1; }
	cp -R user/libc-glue/include/. "$(SDK_TC)/i686-nanos/include/"
	cp kernel/SyscallNr.h          "$(SDK_TC)/i686-nanos/include/SyscallNr.h"
	cp $(BINFOLDER)libc.ndl.a      "$(SDK_TC)/i686-nanos/lib/libc.a"
	cp $(BINFOLDER)libc.ndl        "$(SDK_TC)/i686-nanos/lib/libc.ndl"
	docker run --rm \
	  -v "$(SDK_TC)":/work/toolchain -v "$(DROPBEAR_PORT)":/work/port -v "$(NANOS_SDK)":/sdk \
	  -e SDK=/sdk -e PATH="/work/toolchain/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
	  -w /work/port nanos-sdk-dev:latest python3 /sdk/port/nanos-port /work/port
	cp "$(DROPBEAR_PORT)/dropbear.nxe" $(BINFOLDER)dropbear.nxe
	@for b in dropbearkey dbclient; do \
	  test -f "$(DROPBEAR_PORT)/$$b.nxe" && cp "$(DROPBEAR_PORT)/$$b.nxe" $(BINFOLDER)$$b.nxe && echo "  staged $$b.nxe" || true; \
	done
	@echo "staged $(BINFOLDER)dropbear.nxe (+ dropbearkey/dbclient) — run 'make image' to install"

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

# NetSurf graphical web browser (optional, external): a full stack of ported libraries (zlib,
# libpng/jpeg, libcurl over the ported OpenSSL, libcss/libdom/libhubbub/...) + the bespoke NanWM
# libnsfb surface backend, all in the separate netsurf-nanos repo. `make netsurf` refreshes the SDK
# sysroot from this checkout, then runs that repo's ordered build-all (each port cross-builds in the
# nanos-sdk-dev container — build-all runs on the HOST and shells into Docker per port, so it is NOT
# run inside a container here), and stages netsurf.nxe + its res/ tree into bin/. `make image`
# installs the /apps/netsurf bundle. Launch it inside NanWM with `-f nanwm`.
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
.PHONY: assets externals bash grep vim bzip2 ping wget git inetd httpd udhcpc   # never confuse these with the assets/ dir or bin/ files
assets:
	@command -v python3 >/dev/null 2>&1 || { echo "need python3 + Pillow for assets"; exit 1; }
	python3 scripts/png2raw.py "$(ART_DIR)/wallpaper.png" $(BINFOLDER)wallpaper.raw 1024x768 --bg 0x0a1020
	python3 scripts/png2raw.py "$(ART_DIR)/logo.png"      $(BINFOLDER)logo.raw      96x96     --bg 0xf4f8fd
	@echo "staged $(BINFOLDER)wallpaper.raw + logo.raw — run 'make image' to install them"

# -cpu Nehalem: expose RDRAND so the kernel CSPRNG seeds from a hardware RNG (the default qemu32
# CPU lacks it — without this flag archHwRandom returns false and the seed is RDTSC-jitter+RTC only).
QEMU_CPU=-cpu Nehalem
# RAM: 512 MiB. The kernel reads the real size from multiboot and lays out its windows above it
# (mmu_x86.cpp), so this is just the QEMU knob — bump it freely (up to ~1 GiB with the current
# window placement). More RAM = bigger kernel heap + a bigger user frame pool.
QEMU_MEM=-m 512

run: image
	qemu-system-i386 $(QEMU_CPU) $(QEMU_MEM) -drive file=$(IMAGE_GRUB2),format=raw $(NIC_NET)

run-iso: iso
	qemu-system-i386 -cdrom nanos.iso

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
	qemu-system-i386 $(QEMU_CPU) $(QEMU_MEM) -drive file=$(IMAGE_GRUB2),format=raw $(NIC_OPTS)

# Tests run in a lightweight NATIVE-arch image (no amd64 emulation -> fast), since
# they need only g++/lcov, not the cross toolchain or GRUB.
TEST_IMAGE=nanos-test
TEST_DOCKER_RUN=docker run --rm -v $(CURDIR):/src -w /src $(TEST_IMAGE)

test-image:
	docker build -t $(TEST_IMAGE) -f docker/Dockerfile.test docker/

test: test-image
	$(TEST_DOCKER_RUN) make _test

coverage: test-image
	$(TEST_DOCKER_RUN) make _coverage

clean:
	$(DOCKER_RUN) make _clean
	-rm -rf iso/ nanos.iso $(IMAGE_GRUB2) coverage/

# Machine-independence guard: the MI layer (init/kernel/mm/fs/lib/drivers) must
# only reach the arch via <arch/...> contracts — never x86 internals. Runs on the
# host (plain grep, no Docker). Fails if any forbidden reference creeps back in.
MI_CHECK_DIRS=init kernel mm fs lib drivers
check-arch:
	@if grep -rnE '#include[[:space:]]*"(Gdt|Idt|Interrupt|IOPort|Paging|PagingControl|AddressSpace|Multiboot)[A-Za-z]*\.h"|\bRegisters\b|\bIRQ[0-9]|__asm__|asm[[:space:]]*\(|asm[[:space:]]+volatile|\b(outb|inb|inw)\b' $(MI_CHECK_DIRS); then \
	   echo "FAIL: machine-dependent reference in MI layer (above)"; exit 1; \
	 else echo "OK: MI layer is arch-clean."; fi

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
VPATH=init:kernel:drivers:fs:fs/ext:mm:lib:net:$(ARCH_VPATH)
# Kernel include path: the MI code dirs + the arch contracts (arch/include) + the
# selected arch's headers (ARCH_INCLUDES) + the freestanding <string.h> in include/.
KINCLUDES=-Iarch/include -Iinit -Ikernel -Idrivers -Ifs -Imm -Ilib -Inet -Iinclude -Iuser/term $(ARCH_INCLUDES)

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

CXXFLAGS=-ffreestanding -nostdlib -nostdinc++ $(KINCLUDES) -Wall --no-exceptions --no-rtti -fno-sized-deallocation -fno-leading-underscore $(KOPTFLAGS)
LDFLAGS=-T$(ARCH_LINKER) -nostdlib -nostartfiles -lgcc
ASFLAGS=

# The repo is bind-mounted from a case-insensitive macOS FS, so <string.h> would
# resolve to lib/String.h (infinite include recursion). Compile from a copy on the
# container's own case-sensitive FS, then copy the kernel back.
KSRC=/tmp/nanos-ksrc

# Full link set as paths under the (bind-mounted) object dir.
OBJECTS=$(addprefix $(BINFOLDER),$(SOURCES))

# Sources are compiled from a case-sensitive copy in $(KSRC) (the macOS bind mount is
# case-insensitive, where <string.h> would collide with lib/String.h). But the OBJECTS
# are written into the bind-mounted /src/bin via a symlink, so they PERSIST across
# container runs. tar preserves source mtimes, so make rebuilds only what changed
# (and, via -MMD dep files, what a changed header reaches) instead of everything.
_all:
	@mkdir -p $(BINFOLDER)
	@rm -rf $(KSRC) && mkdir -p $(KSRC) && \
	 tar -cf - --exclude=.git --exclude=disk --exclude=bin --exclude=iso --exclude=coverage --exclude=tests -C /src . | tar -xf - -C $(KSRC) && \
	 ln -s /src/$(BINFOLDER) $(KSRC)/bin
	$(MAKE) -C $(KSRC) _compile

_compile: $(BINFOLDER)kernel.bin

$(BINFOLDER)kernel.bin: $(OBJECTS)
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS)

$(BINFOLDER)%.o: %.cpp
	$(CXX) -c $(CXXFLAGS) -MMD -MP $< -o $@
# The shared VT engine (user/term/vt.c) compiled with KERNEL flags for FbConsole. A distinct
# object name (vtk.o) keeps it separate from the userland bin/vt.o that nterm/nwterm link.
$(BINFOLDER)vtk.o: user/term/vt.c
	@mkdir -p $(BINFOLDER)
	$(CXX) -c $(CXXFLAGS) -MMD -MP $< -o $@
$(BINFOLDER)%.o: %.s
	$(AS) $(ASFLAGS) $< -o $@
$(BINFOLDER)%.o: %.S
	nasm -f elf $< -o $@

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
	  printf "rm /apps/$$p/$$p.nxe\nwrite $(BINFOLDER)$$p.nxe /apps/$$p/$$p.nxe\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
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
	  printf "rm /apps/bash/bash.nxe\nwrite $(BINFOLDER)bash.nxe /apps/bash/bash.nxe\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
	  printf "rm /bin/bash.nxe\n" | debugfs -w "$(IMAGE_GRUB2_PART)" 2>/dev/null; \
	  printf "symlink /bin/bash.nxe /apps/bash/bash.nxe\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
	fi
	# vim (optional, external): built by the nanos-sdk port and staged into bin/vim.nxe, same
	# pattern as bash. Its runtime (syntax/help/etc.) is a `data` entry the port driver installs
	# into the bundle; vim itself runs without it (`-u NONE`). Skipped if bin/vim.nxe is absent.
	if [ -f $(BINFOLDER)vim.nxe ]; then \
	  printf "mkdir /apps/vim\n" | debugfs -w "$(IMAGE_GRUB2_PART)" 2>/dev/null; \
	  printf "rm /apps/vim/vim.nxe\nwrite $(BINFOLDER)vim.nxe /apps/vim/vim.nxe\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
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
	  printf "rm /nanos/bin/ping.nxe\nwrite $(BINFOLDER)ping.nxe /nanos/bin/ping.nxe\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
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
	# Launch inside NanWM with `-f nanwm` (selects the NanWM libnsfb surface). Skipped if absent.
	if [ -f $(BINFOLDER)netsurf.nxe ]; then \
	  printf "mkdir /apps/netsurf\n" | debugfs -w "$(IMAGE_GRUB2_PART)" 2>/dev/null; \
	  printf "rm /apps/netsurf/netsurf.nxe\nwrite $(BINFOLDER)netsurf.nxe /apps/netsurf/netsurf.nxe\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
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
	# Desktop artwork (optional, `make assets`): the branded wallpaper + logo as flat 32bpp surfaces
	# under /nanos/share. The compositor blits wallpaper.raw as the background; About shows logo.raw.
	if [ -f $(BINFOLDER)wallpaper.raw ]; then \
	  printf "rm /nanos/share/wallpaper.raw\nwrite $(BINFOLDER)wallpaper.raw /nanos/share/wallpaper.raw\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
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
	-rm -rf $(BINFOLDER)libimp

# ----------------------------------------------------------------------------
# Userland: .nxe programs link against ported picolibc + our syscall glue (own
# linker script/base 0x400000, NOT in kernel SOURCES). picolibc headers come via
# -isystem; SyscallNr.h via -Ikernel. No <string.h> clash (lib/ is not on the path).
# ----------------------------------------------------------------------------
PICOLIBC=/opt/picolibc/i686-elf
SBASE=user/third_party/sbase
# kernel/ is on -iquote (not -I): SyscallNr.h is a "quoted" include, and this keeps the
# new kernel/Signal.h from shadowing picolibc's <signal.h> on the case-insensitive macOS
# bind mount (kernel/Signal.h == <signal.h> under -I, which broke the userland build).
USER_CFLAGS=-ffreestanding -isystem $(PICOLIBC)/include -iquote kernel -Iuser -Iuser/libnw -Iuser/nwm -Iuser/libnwui -Iuser/term -Iuser/libc-glue/include -I$(SBASE) -D_DEFAULT_SOURCE -include user/libc-glue/compat-decls.h -Wall -fno-pic -fno-stack-protector $(UOPTFLAGS)
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
LIBUTF_OBJS=$(patsubst $(SBASE)/libutf/%.c,$(BINFOLDER)%.o,$(wildcard $(SBASE)/libutf/*.c))
GLUE_LS=$(BINFOLDER)dirent.o $(BINFOLDER)pwd_grp.o
# Programs built. Placement (see _image): init -> /nanos/core (PID 1); system utilities
# -> /nanos/bin; non-system apps (games/demos/tests) -> /apps.
USER_PROGS=init nsh cat ls mkdir rmdir pwd sigtest fbtest timetest brktest inputtest fstest free usedll pipetest forkmany orphan clonetest ptytest nterm tuitest racetest envtest mmaptest mousetest doom nwm nwnote nwform rustform nwexp nwset nwterm nwabout crashtest socktest pingtest nettest unixtest tcpsrv nanologin dhcpcfg randhex errnotest pthrtest pthrstress pfract
SYS_PROGS=nsh cat ls mkdir rmdir pwd free nwm socktest pingtest nettest unixtest tcpsrv nanologin randhex errnotest
APP_PROGS=sigtest fbtest timetest brktest inputtest fstest usedll pipetest forkmany orphan clonetest ptytest nterm tuitest racetest envtest mmaptest mousetest doom nwnote nwform rustform nwexp nwset nwterm nwabout crashtest pthrtest pthrstress pfract
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
$(BINFOLDER)%.o: $(SBASE)/%.c
	$(CXX) $(USER_CFLAGS) $(DYNHDR) -MMD -MP -c $< -o $@
$(BINFOLDER)%.o: $(SBASE)/libutil/%.c
	$(CXX) $(USER_CFLAGS) $(DYNHDR) -MMD -MP -c $< -o $@
$(BINFOLDER)%.o: $(SBASE)/libutf/%.c
	$(CXX) $(USER_CFLAGS) $(DYNHDR) -MMD -MP -c $< -o $@
$(BINFOLDER)%.o: user/%.S
	@mkdir -p $(BINFOLDER)
	nasm -f elf $< -o $@
$(BINFOLDER)%.o: user/libc-glue/%.S
	@mkdir -p $(BINFOLDER)
	nasm -f elf $< -o $@
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
# NanWM compositor (user/nwm) + client demos (user/nwnote). DYNHDR (libc.ndl import shim) —
# these are dynamically-linked programs like the rest of userland.
$(BINFOLDER)%.o: user/nwm/%.c
	@mkdir -p $(BINFOLDER)
	$(CXX) $(USER_CFLAGS) $(DYNHDR) -MMD -MP -c $< -o $@
$(BINFOLDER)%.o: user/nwnote/%.c
	@mkdir -p $(BINFOLDER)
	$(CXX) $(USER_CFLAGS) $(DYNHDR) -MMD -MP -c $< -o $@
$(BINFOLDER)%.o: user/nwform/%.c
	@mkdir -p $(BINFOLDER)
	$(CXX) $(USER_CFLAGS) $(DYNHDR) -MMD -MP -c $< -o $@
$(BINFOLDER)%.o: user/nwexp/%.c
	@mkdir -p $(BINFOLDER)
	$(CXX) $(USER_CFLAGS) $(DYNHDR) -MMD -MP -c $< -o $@
$(BINFOLDER)%.o: user/nwset/%.c
	@mkdir -p $(BINFOLDER)
	$(CXX) $(USER_CFLAGS) $(DYNHDR) -MMD -MP -c $< -o $@
$(BINFOLDER)%.o: user/nwabout/%.c
	@mkdir -p $(BINFOLDER)
	$(CXX) $(USER_CFLAGS) $(DYNHDR) -MMD -MP -c $< -o $@
$(BINFOLDER)%.o: user/nwterm/%.c
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

# Generic DYNAMIC link: every program links its objects + the import library (NO static
# libc) and declares "needed: libc.ndl", so the loader maps libc.ndl and binds its imports
# by name. `--emit-relocs` keeps the R_386_32 relocations so mknx can build the relocation
# table (the .nxe loads at any base). Each program below just declares its object prereqs.
$(BINFOLDER)%.nxe: $(MKNX)
	$(LD) -nostdlib -Wl,--emit-relocs -T user/nx.ld -o $(@:.nxe=.elf) $(filter %.o,$^) $(filter %.a,$^) -lgcc
	$(MKNX) $(@:.nxe=.elf) $@ --need libc.ndl

# Per-program object sets: DYN_GLUE (crt0+nxhdr) + program objects + the libc import
# library (an ARCHIVE — the linker pulls only the members the program references, so it
# imports just the symbols it uses). libc.ndl is a prereq so it is built/shipped. Doom +
# usedll have explicit rules (extra math / a second needed library).
DYN_DEPS=$(DYN_GLUE) $(BINFOLDER)libc.ndl.a $(BINFOLDER)libc.ndl
$(BINFOLDER)init.nxe:      $(DYN_DEPS) $(BINFOLDER)init.o
$(BINFOLDER)nsh.nxe:       $(DYN_DEPS) $(BINFOLDER)nsh.o
$(BINFOLDER)free.nxe:      $(DYN_DEPS) $(BINFOLDER)free.o
$(BINFOLDER)cat.nxe:       $(DYN_DEPS) $(BINFOLDER)cat.o $(SBASE_UTIL_CAT)
$(BINFOLDER)ls.nxe:        $(DYN_DEPS) $(BINFOLDER)ls.o $(SBASE_UTIL_LS) $(LIBUTF_OBJS) $(GLUE_LS)
$(BINFOLDER)mkdir.nxe:     $(DYN_DEPS) $(BINFOLDER)mkdir.o $(SBASE_UTIL_MKDIR)
$(BINFOLDER)rmdir.nxe:     $(DYN_DEPS) $(BINFOLDER)rmdir.o $(SBASE_UTIL_RMDIR)
$(BINFOLDER)pwd.nxe:       $(DYN_DEPS) $(BINFOLDER)pwd.o $(SBASE_UTIL_PWD)
$(BINFOLDER)sigtest.nxe:   $(DYN_DEPS) $(BINFOLDER)sigtest.o
$(BINFOLDER)crashtest.nxe: $(DYN_DEPS) $(BINFOLDER)crashtest.o
$(BINFOLDER)socktest.nxe:  $(DYN_DEPS) $(BINFOLDER)socktest.o
$(BINFOLDER)pingtest.nxe:  $(DYN_DEPS) $(BINFOLDER)pingtest.o
$(BINFOLDER)nettest.nxe:   $(DYN_DEPS) $(BINFOLDER)nettest.o
$(BINFOLDER)unixtest.nxe:  $(DYN_DEPS) $(BINFOLDER)unixtest.o
$(BINFOLDER)tcpsrv.nxe:    $(DYN_DEPS) $(BINFOLDER)tcpsrv.o
$(BINFOLDER)nanologin.nxe: $(DYN_DEPS) $(BINFOLDER)nanologin.o
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
$(BINFOLDER)pthrtest.nxe: $(DYN_DEPS) $(BINFOLDER)pthrtest.o
$(BINFOLDER)pthrstress.nxe: $(DYN_DEPS) $(BINFOLDER)pthrstress.o
$(BINFOLDER)pfract.nxe:    $(DYN_DEPS) $(BINFOLDER)pfract.o
$(BINFOLDER)ptytest.nxe:   $(DYN_DEPS) $(BINFOLDER)ptytest.o
$(BINFOLDER)nterm.nxe:     $(DYN_DEPS) $(BINFOLDER)nterm.o $(BINFOLDER)vt.o $(BINFOLDER)vtfont.o
# NanWM: the compositor (statically links the pure cores + gfx) and the nwnote demo client
# (statically links libnw + the shared codec/gfx). Both dynamic-link libc.ndl via DYN_DEPS.
$(BINFOLDER)nwm.nxe:       $(DYN_DEPS) $(BINFOLDER)nwm.o $(BINFOLDER)nwm_core.o $(BINFOLDER)nw_compose.o $(BINFOLDER)nwproto.o $(BINFOLDER)nw_gfx.o $(BINFOLDER)vtfont.o
# nwnote links the libnw import library (+ libc.ndl.a for crt0's exit thunk) and declares only
# --need libnw.ndl; the recursive loader auto-loads libc.ndl (libnw's dependency), exactly like
# a Windows app that links user32 and gets ntdll transitively.
$(BINFOLDER)nwnote.nxe: $(DYN_GLUE) $(BINFOLDER)nwnote.o $(BINFOLDER)libnw.ndl.a $(BINFOLDER)libc.ndl.a $(BINFOLDER)libnw.ndl $(BINFOLDER)libc.ndl $(MKNX)
	$(LD) -nostdlib -Wl,--emit-relocs -T user/nx.ld -o $(BINFOLDER)nwnote.elf $(DYN_GLUE) $(BINFOLDER)nwnote.o $(BINFOLDER)libnw.ndl.a $(BINFOLDER)libc.ndl.a -lgcc
	$(MKNX) $(BINFOLDER)nwnote.elf $@ --need libnw.ndl
# nwform uses ONLY the toolkit (+ libc for snprintf/exit); --need libnwui.ndl pulls the whole
# chain libnwui->libnw->libc via the recursive loader.
$(BINFOLDER)nwform.nxe: $(DYN_GLUE) $(BINFOLDER)nwform.o $(BINFOLDER)libnwui.ndl.a $(BINFOLDER)libc.ndl.a $(BINFOLDER)libnwui.ndl $(BINFOLDER)libnw.ndl $(BINFOLDER)libc.ndl $(MKNX)
	$(LD) -nostdlib -Wl,--emit-relocs -T user/nx.ld -o $(BINFOLDER)nwform.elf $(DYN_GLUE) $(BINFOLDER)nwform.o $(BINFOLDER)libnwui.ndl.a $(BINFOLDER)libc.ndl.a -lgcc
	$(MKNX) $(BINFOLDER)nwform.elf $@ --need libnwui.ndl
# nwexp: the file explorer — same toolkit-only chain as nwform (--need libnwui.ndl).
$(BINFOLDER)nwexp.nxe: $(DYN_GLUE) $(BINFOLDER)nwexp.o $(BINFOLDER)libnwui.ndl.a $(BINFOLDER)libc.ndl.a $(BINFOLDER)libnwui.ndl $(BINFOLDER)libnw.ndl $(BINFOLDER)libc.ndl $(MKNX)
	$(LD) -nostdlib -Wl,--emit-relocs -T user/nx.ld -o $(BINFOLDER)nwexp.elf $(DYN_GLUE) $(BINFOLDER)nwexp.o $(BINFOLDER)libnwui.ndl.a $(BINFOLDER)libc.ndl.a -lgcc
	$(MKNX) $(BINFOLDER)nwexp.elf $@ --need libnwui.ndl
# nwset: the Settings demo — toolkit-only chain like nwform/nwexp.
$(BINFOLDER)nwset.nxe: $(DYN_GLUE) $(BINFOLDER)nwset.o $(BINFOLDER)libnwui.ndl.a $(BINFOLDER)libc.ndl.a $(BINFOLDER)libnwui.ndl $(BINFOLDER)libnw.ndl $(BINFOLDER)libc.ndl $(MKNX)
	$(LD) -nostdlib -Wl,--emit-relocs -T user/nx.ld -o $(BINFOLDER)nwset.elf $(DYN_GLUE) $(BINFOLDER)nwset.o $(BINFOLDER)libnwui.ndl.a $(BINFOLDER)libc.ndl.a -lgcc
	$(MKNX) $(BINFOLDER)nwset.elf $@ --need libnwui.ndl
# nwabout: "About This Computer" — toolkit-only chain like nwset.
$(BINFOLDER)nwabout.nxe: $(DYN_GLUE) $(BINFOLDER)nwabout.o $(BINFOLDER)libnwui.ndl.a $(BINFOLDER)libc.ndl.a $(BINFOLDER)libnwui.ndl $(BINFOLDER)libnw.ndl $(BINFOLDER)libc.ndl $(MKNX)
	$(LD) -nostdlib -Wl,--emit-relocs -T user/nx.ld -o $(BINFOLDER)nwabout.elf $(DYN_GLUE) $(BINFOLDER)nwabout.o $(BINFOLDER)libnwui.ndl.a $(BINFOLDER)libc.ndl.a -lgcc
	$(MKNX) $(BINFOLDER)nwabout.elf $@ --need libnwui.ndl
# nwterm: the real windowed Terminal — a raw libnw client running nsh on a pty, with the shared
# VT engine (vt.o). --need libnw.ndl.
$(BINFOLDER)nwterm.nxe: $(DYN_GLUE) $(BINFOLDER)nwterm.o $(BINFOLDER)vt.o $(BINFOLDER)libnw.ndl.a $(BINFOLDER)libc.ndl.a $(BINFOLDER)libnw.ndl $(BINFOLDER)libc.ndl $(MKNX)
	$(LD) -nostdlib -Wl,--emit-relocs -T user/nx.ld -o $(BINFOLDER)nwterm.elf $(DYN_GLUE) $(BINFOLDER)nwterm.o $(BINFOLDER)vt.o $(BINFOLDER)libnw.ndl.a $(BINFOLDER)libc.ndl.a -lgcc
	$(MKNX) $(BINFOLDER)nwterm.elf $@ --need libnw.ndl
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
  $(BINFOLDER)dirent.o $(BINFOLDER)pwd_grp.o $(BINFOLDER)posixstubs.o $(BINFOLDER)sockets.o $(BINFOLDER)resolv.o \
  $(BINFOLDER)resolv_parse.o $(BINFOLDER)stdio_ext.o $(BINFOLDER)ptyutil.o $(BINFOLDER)ifname.o \
  $(BINFOLDER)crypt.o $(BINFOLDER)tls.o $(BINFOLDER)retarget_lock.o $(BINFOLDER)getdelim.o $(LIBC_PTHREAD_OBJS)
# Phase 4: the vendored musl pthread internals (user/libc-glue/pthread/, musl 1.2.5),
# adapted to NanOS syscalls (int 0x80, Linux i386 numbers) + the picolibc TCB. Task 4.1
# brings in only the futex/clone/TLS primitives (no pthread_create yet); they link into
# libc.ndl. The vendored .c/.s files are built by the dedicated user/libc-glue/pthread rules
# below (extra -I for the musl headers + the weak_alias/hidden force-include).
LIBC_PTHREAD_OBJS=$(BINFOLDER)__wait.o $(BINFOLDER)__timedwait.o $(BINFOLDER)__lock.o \
  $(BINFOLDER)pthread_self.o $(BINFOLDER)default_attr.o $(BINFOLDER)nanos_glue.o \
  $(BINFOLDER)clone.o $(BINFOLDER)__set_thread_area.o \
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
$(BINFOLDER)libc.ndl: $(BINFOLDER)libc.elf $(MKNX)
	$(MKNX) $(BINFOLDER)libc.elf $@ --dll --export-all

# Import library libc.ndl.a: an ARCHIVE with ONE member per export — a `name: jmp
# [__imp_name]` thunk + IAT slot for each function, a slot-only member for each data symbol
# (stdout/errno, reached via nx-dllimport.h). A program links the archive instead of static
# picolibc; the linker pulls ONLY referenced members, so the program imports just what it
# uses (not all ~130 symbols). mknx writes one .s per symbol into a dir; we assemble + ar.
$(BINFOLDER)libc.ndl.a: $(BINFOLDER)libc.elf $(MKNX)
	rm -rf $(BINFOLDER)libimp && mkdir -p $(BINFOLDER)libimp
	$(MKNX) $(BINFOLDER)libc.elf $(BINFOLDER)libimp --implib --export-all --soname libc.ndl
	for f in $(BINFOLDER)libimp/*.s; do nasm -f elf "$$f" -o "$${f%.s}.o"; done
	rm -f $@ && ar rcs $@ $(BINFOLDER)libimp/*.o

# ---- libnw.ndl: the shared window-client library (the user32/gdi32 of NanWM) ----
# A real shared library (like greet.ndl/libc.ndl): the protocol codec + gfx + client API in
# ONE relocatable module, exporting the nw_* API. It USES libc (malloc/read/write/poll), so
# it links the libc import library and declares `--need libc.ndl`; the recursive loader
# (kernel/DynLoader.cpp) pulls libc.ndl in automatically when a client loads libnw.ndl.
LIBNW_OBJS=$(BINFOLDER)libnw.o $(BINFOLDER)nwproto.o $(BINFOLDER)nw_gfx.o $(BINFOLDER)vtfont.o
$(BINFOLDER)libnw.elf: $(BINFOLDER)nxhdr.o $(LIBNW_OBJS) $(BINFOLDER)libc.ndl.a
	$(LD) -nostdlib -Wl,--emit-relocs -T user/dll.ld -o $@ $(BINFOLDER)nxhdr.o $(LIBNW_OBJS) $(BINFOLDER)libc.ndl.a -lgcc
$(BINFOLDER)libnw.ndl: $(BINFOLDER)libnw.elf $(MKNX)
	$(MKNX) $(BINFOLDER)libnw.elf $@ --dll --export-all --need libc.ndl
# Import library libnw.ndl.a (clients link this; the nw_* thunks bind to libnw.ndl at load).
$(BINFOLDER)libnw.ndl.a: $(BINFOLDER)libnw.elf $(MKNX)
	rm -rf $(BINFOLDER)libnwimp && mkdir -p $(BINFOLDER)libnwimp
	$(MKNX) $(BINFOLDER)libnw.elf $(BINFOLDER)libnwimp --implib --export-all --soname libnw.ndl
	for f in $(BINFOLDER)libnwimp/*.s; do nasm -f elf "$$f" -o "$${f%.s}.o"; done
	rm -f $@ && ar rcs $@ $(BINFOLDER)libnwimp/*.o

# ---- libnwui.ndl: the UI toolkit (the comctl32 of NanWM) ----
# Composable widget tree + flex layout + paint, exporting the nwui_* API. Uses libnw (gfx) AND
# libc (malloc); links both import libraries and declares both needs. A client that --need
# libnwui.ndl gets the whole chain app->libnwui->libnw->libc via the recursive loader.
LIBNWUI_OBJS=$(BINFOLDER)nwui_core.o $(BINFOLDER)nwui_paint.o $(BINFOLDER)nwui.o
$(BINFOLDER)libnwui.elf: $(BINFOLDER)nxhdr.o $(LIBNWUI_OBJS) $(BINFOLDER)libnw.ndl.a $(BINFOLDER)libc.ndl.a
	$(LD) -nostdlib -Wl,--emit-relocs -T user/dll.ld -o $@ $(BINFOLDER)nxhdr.o $(LIBNWUI_OBJS) $(BINFOLDER)libnw.ndl.a $(BINFOLDER)libc.ndl.a -lgcc
$(BINFOLDER)libnwui.ndl: $(BINFOLDER)libnwui.elf $(MKNX)
	$(MKNX) $(BINFOLDER)libnwui.elf $@ --dll --export-all --need libnw.ndl --need libc.ndl
$(BINFOLDER)libnwui.ndl.a: $(BINFOLDER)libnwui.elf $(MKNX)
	rm -rf $(BINFOLDER)libnwuiimp && mkdir -p $(BINFOLDER)libnwuiimp
	$(MKNX) $(BINFOLDER)libnwui.elf $(BINFOLDER)libnwuiimp --implib --export-all --soname libnwui.ndl
	for f in $(BINFOLDER)libnwuiimp/*.s; do nasm -f elf "$$f" -o "$${f%.s}.o"; done
	rm -f $@ && ar rcs $@ $(BINFOLDER)libnwuiimp/*.o

# All programs + shared libraries (init -> /nanos/core, the rest -> /nanos/bin, libs -> /nanos/lib).
_userland: $(addprefix $(BINFOLDER),$(addsuffix .nxe,$(USER_PROGS))) $(addprefix $(BINFOLDER),$(USER_LIBS_NDL))

# ----------------------------------------------------------------------------
# Kernel modules (nkext): loadable drivers built SEPARATELY from kernel.bin, shipped to
# /nanos/kext and loaded at boot (kernel/KextLoader). A kext is an NxFormat module (like a
# .ndl) but its imports resolve to the KERNEL export table (KernelExports), not libc.ndl. Kext
# code is ring-0 freestanding C++ (no picolibc); -Iinclude gives the freestanding string.h and
# we deliberately omit -Ilib so the macOS case-insensitivity trap can't bite when building in
# /src. (See docs/filesystem.md: /nanos/kext.)
KEXT_CFLAGS=-ffreestanding -nostdlib -nostdinc++ --no-exceptions --no-rtti \
  -fno-sized-deallocation -fno-leading-underscore -fno-pic -fno-stack-protector \
  -Iarch/include -Ikernel -Idrivers -Iinclude $(KOPTFLAGS)

# Kernel import library for kexts, GENERATED from kexports.def (single source of truth): a
# module links these thunks/slots (__imp_knx_* in section .nxlib.kernel), which mknx turns
# into "kernel"-scoped imports the loader binds against KernelExports.
$(BINFOLDER)kimports.S: kernel/kexports.def
	@mkdir -p $(BINFOLDER)
	@awk 'BEGIN{print "[BITS 32]"; print "section .nxlib.kernel progbits alloc write align=4"} \
	  /^KX\(/{ n=$$0; sub(/^KX\(/,"",n); sub(/\).*/,"",n); k[++c]=n; print "[GLOBAL __imp_" n "]"; print "__imp_" n ": dd 0" } \
	  END{ print "section .text"; for(i=1;i<=c;i++){ print "[GLOBAL " k[i] "]"; print k[i] ": jmp [__imp_" k[i] "]" } }' $< > $@
$(BINFOLDER)kimports.o: $(BINFOLDER)kimports.S
	nasm -f elf $< -o $@

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

# Per-kext link: nxhdr placeholder + generated kernel import stub + kext runtime + objects,
# linked at the kext base with relocations kept (--emit-relocs), then mknx -> .nkext.
KEXT_GLUE=$(BINFOLDER)nxhdr.o $(BINFOLDER)kimports.o $(BINFOLDER)kext_rt.o
$(BINFOLDER)mouse.nkext: $(KEXT_GLUE) $(BINFOLDER)MouseDevice.o $(BINFOLDER)mouse_ps2.o $(MKNX) kext/kext.ld
	$(LD) -nostdlib -Wl,--emit-relocs -T kext/kext.ld -o $(@:.nkext=.elf) \
	  $(KEXT_GLUE) $(BINFOLDER)MouseDevice.o $(BINFOLDER)mouse_ps2.o -lgcc
	$(MKNX) $(@:.nkext=.elf) $@
$(BINFOLDER)kbd.nkext: $(KEXT_GLUE) $(BINFOLDER)kbd_ps2.o $(MKNX) kext/kext.ld
	$(LD) -nostdlib -Wl,--emit-relocs -T kext/kext.ld -o $(@:.nkext=.elf) \
	  $(KEXT_GLUE) $(BINFOLDER)kbd_ps2.o -lgcc
	$(MKNX) $(@:.nkext=.elf) $@
$(BINFOLDER)e1000.nkext: $(KEXT_GLUE) $(BINFOLDER)e1000.o $(MKNX) kext/kext.ld
	$(LD) -nostdlib -Wl,--emit-relocs -T kext/kext.ld -o $(@:.nkext=.elf) \
	  $(KEXT_GLUE) $(BINFOLDER)e1000.o -lgcc
	$(MKNX) $(@:.nkext=.elf) $@

KEXTS=kbd mouse e1000
_kext: $(addprefix $(BINFOLDER),$(addsuffix .nkext,$(KEXTS)))

# Doom (doomgeneric). Old-C source needs -fcommon (GCC 10+ defaults to -fno-common, which
# breaks Doom's tentative globals) and warnings off; -DNORMALUNIX -DLINUX select the POSIX
# code paths; -lm for the renderer's trig/sqrt. Our platform layer (doomgeneric_nanos.c)
# replaces the shipped backends. Built as the `doom` program in USER_PROGS.
DOOM_DIR=user/third_party/doomgeneric
DOOM_CFLAGS=-ffreestanding -isystem $(PICOLIBC)/include -iquote kernel -Iuser -I$(DOOM_DIR) -D_DEFAULT_SOURCE -DNORMALUNIX -DLINUX -include user/libc-glue/compat-decls.h -include user/libc-glue/nx-dllimport.h -w -fcommon -fno-pic -fno-stack-protector $(UOPTFLAGS)
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
$(BINFOLDER)doom.nxe: $(DYN_GLUE) $(DOOM_OBJS) $(BINFOLDER)doomgeneric_nanos.o $(BINFOLDER)libc.ndl.a $(BINFOLDER)libc.ndl $(MKNX)
	$(LD) -nostdlib -Wl,--emit-relocs -T user/nx.ld -o $(BINFOLDER)doom.elf $(DYN_GLUE) $(DOOM_OBJS) $(BINFOLDER)doomgeneric_nanos.o $(BINFOLDER)libc.ndl.a -L$(PICOLIBC)/lib -lc -lgcc
	$(MKNX) $(BINFOLDER)doom.elf $(BINFOLDER)doom.nxe --need libc.ndl

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
# Host include path: code dirs only, deliberately WITHOUT -Iinclude so that
# <string.h> resolves to libc (not the freestanding include/string.h).
HINCLUDES=-Iarch/include -Ikernel -Idrivers -Ifs -Imm -Ilib -Inet -Iarch/x86/boot -Iarch/x86/mm -Ikext/mouse -Iuser/libnw -Iuser/nwm -Iuser/libnwui -Iuser/term -Iuser/libc-glue
HOST_CXXFLAGS=-std=c++17 -O0 -g $(HINCLUDES) -Wall --coverage
TEST_BIN=/tmp/nanos_tests
TEST_SRCS=$(wildcard tests/*.cpp)
# Modules under test (grown as layers are added). Header-only modules contribute
# coverage via the .h patterns below.
TEST_MODULES=drivers/RamBlockDevice.cpp drivers/DeviceManager.cpp drivers/Console.cpp fs/Vfs.cpp fs/ExtFilesystem.cpp fs/SynthFs.cpp fs/RamFs.cpp fs/ext/Crc32c.cpp fs/ext/BlockCache.cpp fs/ext/ExtCsum.cpp fs/ext/ExtAllocator.cpp fs/ext/Journal.cpp kernel/Syscall.cpp kernel/NxeLoader.cpp kernel/KeyDecoder.cpp kernel/Scheduler.cpp kernel/Process.cpp kernel/Signal.cpp kernel/Futex.cpp kernel/Csprng.cpp lib/String.cpp
TEST_MODULES+= arch/x86/boot/MultibootMmap.cpp mm/FrameAllocator.cpp mm/Heap.cpp arch/x86/mm/AddressSpace.cpp
TEST_MODULES+= drivers/Framebuffer.cpp drivers/Font8x16.cpp drivers/FbConsole.cpp drivers/Fbdev.cpp drivers/KeyboardDevice.cpp drivers/Pty.cpp
TEST_MODULES+= kext/mouse/MouseDevice.cpp   # MI half of the mouse kext (PS/2 decode -> evdev)
# NanWM (window server) pure cores — userland C, host-tested as C++ (g++ treats .c as C++).
# vtfont.c supplies the shared nx_font8x16 the gfx rasterizer draws with.
TEST_MODULES+= user/libnw/nwproto.c user/libnw/nw_gfx.c user/term/vtfont.c
TEST_MODULES+= user/nwm/nwm_core.c user/nwm/nw_compose.c
TEST_MODULES+= user/libnwui/nwui_core.c   # the pure UI-toolkit core (tree/layout/events)
TEST_MODULES+= user/term/vt.c             # the pure VT/ANSI terminal engine (shared by nterm/nwterm)
TEST_MODULES+= kernel/Pci.cpp             # MI PCI enumeration/BAR decode (mock config-space backend)
TEST_MODULES+= net/Net.cpp net/NetBuf.cpp net/NetDevice.cpp net/Loopback.cpp   # MI net core
TEST_MODULES+= net/Ether.cpp net/Arp.cpp net/Ip.cpp net/Route.cpp net/Icmp.cpp   # L2 + ARP + IPv4 + ICMP
TEST_MODULES+= net/Socket.cpp net/Udp.cpp net/Raw.cpp net/Tcp.cpp net/Packet.cpp net/Unix.cpp   # socket layer + UDP + RAW + TCP + AF_PACKET + AF_UNIX
TEST_MODULES+= net/NetProc.cpp net/NetStats.cpp   # /proc/net renderers + SNMP counters (FAZA 14)
TEST_MODULES+= user/libc-glue/resolv_parse.c   # pure resolver parsers (resolv.conf/services/protocols/DNS, FAZA E)
TEST_MODULES+= user/libc-glue/crypt.c          # crypt(3) SHA-512 ($6$) — known-answer vs openssl
# lcov patterns selecting the modules whose coverage is gated (String is support).
COV_PATTERNS="*/RamBlockDevice.*" "*/DeviceManager.*" "*/Vfs.*" "*/ExtFilesystem.*" "*/Ext2Filesystem.*" "*/Ext4Filesystem.*" "*/ExtAllocator.*" "*/BlockCache.*" "*/ExtCsum.*" "*/Crc32c.*" "*/Journal.*" "*/SynthFs.*" "*/RamFs.*" "*/Syscall.*" "*/NxeLoader.*" "*/KeyDecoder.*" "*/Process.*" "*/Signal.*" "*/Futex.*" "*/Csprng.*" "*/Framebuffer.*" "*/FbConsole.*" "*/Fbdev.*" "*/KeyboardDevice.*" "*/Pty.*" "*/MouseDevice.*" "*/MultibootMmap.*" "*/FrameAllocator.*" "*/Heap.*" "*/AddressSpace.*" "*/nwproto.*" "*/nw_gfx.*" "*/nwm_core.*" "*/nw_compose.*" "*/nwui_core.*" "*/vt.*" "*/Pci.*" "*/Net.*" "*/NetBuf.*" "*/NetDevice.*" "*/Loopback.*" "*/Ether.*" "*/Arp.*" "*/Ip.*" "*/Route.*" "*/Icmp.*" "*/Socket.*" "*/Udp.*" "*/Raw.*" "*/Tcp.*" "*/Packet.*" "*/Unix.*" "*/NetProc.*" "*/NetStats.*" "*/resolv_parse.*" "*/crypt.*" "*/GdtBase.*"
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

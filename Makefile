# Architecture selection: machine-dependent knobs come from arch/$(ARCH)/arch.mk
# (CROSS, ARCH_VPATH, ARCH_INCLUDES, ARCH_LINKER, ARCH_SOURCES).
ARCH ?= x86
include arch/$(ARCH)/arch.mk

# Machine-independent objects (portable across architectures).
MI_SOURCES=kmain.o Kernel.o Console.o ExtFilesystem.o SynthFs.o RamFs.o RamBlockDevice.o DeviceManager.o Vfs.o
MI_SOURCES+= Crc32c.o BlockCache.o ExtCsum.o ExtAllocator.o Journal.o
MI_SOURCES+= Framebuffer.o Font8x16.o FbConsole.o Fbdev.o Fb0Device.o KeyboardDevice.o Pty.o
MI_SOURCES+= Syscall.o SyscallDispatch.o NxeLoader.o Exec.o DynLoader.o KernelExports.o KextLoader.o FrameAllocator.o KeyDecoder.o Scheduler.o Process.o Signal.o
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

run: image
	qemu-system-i386 -drive file=$(IMAGE_GRUB2),format=raw

run-iso: iso
	qemu-system-i386 -cdrom nanos.iso

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
VPATH=init:kernel:drivers:fs:fs/ext:mm:lib:$(ARCH_VPATH)
# Kernel include path: the MI code dirs + the arch contracts (arch/include) + the
# selected arch's headers (ARCH_INCLUDES) + the freestanding <string.h> in include/.
KINCLUDES=-Iarch/include -Iinit -Ikernel -Idrivers -Ifs -Imm -Ilib -Iinclude $(ARCH_INCLUDES)

CXXFLAGS=-ffreestanding -nostdlib -nostdinc++ $(KINCLUDES) -Wall --no-exceptions --no-rtti -fno-sized-deallocation -fno-leading-underscore
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
	fi
	# Doom's shareware IWAD is a data file inside the doom app bundle (its layer -iwad's it).
	printf "rm /apps/doom/doom1.wad\nwrite disk/doom1.wad /apps/doom/doom1.wad\n" | debugfs -w "$(IMAGE_GRUB2_PART)"
	# terminfo database: the compiled xterm-256color entry (matches TERM), shipped under
	# /nanos/share/terminfo so a future ncurses finds it via TERMINFO. Copied from the build
	# container's ncurses (ncurses-base); the on-disk path is dir/<first-letter>/<name>.
	printf "rm /nanos/share/terminfo/x/xterm-256color\nwrite /usr/share/terminfo/x/xterm-256color /nanos/share/terminfo/x/xterm-256color\n" | debugfs -w "$(IMAGE_GRUB2_PART)"

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
USER_CFLAGS=-ffreestanding -isystem $(PICOLIBC)/include -iquote kernel -Iuser -Iuser/libnw -Iuser/nwm -Iuser/libnwui -Iuser/term -Iuser/libc-glue/include -I$(SBASE) -D_DEFAULT_SOURCE -include user/libc-glue/compat-decls.h -Wall -fno-pic -fno-stack-protector
USER_LIBS=-L$(PICOLIBC)/lib -lc -lgcc
# Shared per-program objects: startup, .nxe header, the picolibc syscall glue, and
# the userland cwd layer (syscalls.o's path resolver lives in cwd.o).
USER_GLUE=$(BINFOLDER)crt0.o $(BINFOLDER)sigtramp.o $(BINFOLDER)nxhdr.o $(BINFOLDER)syscalls.o $(BINFOLDER)cwd.o
# Per-program libutil subsets and the libutf objects (ls needs UTF + dir/pwd glue).
SBASE_UTIL_CAT=$(BINFOLDER)eprintf.o $(BINFOLDER)concat.o $(BINFOLDER)writeall.o
SBASE_UTIL_LS=$(BINFOLDER)eprintf.o $(BINFOLDER)ealloc.o $(BINFOLDER)reallocarray.o $(BINFOLDER)human.o $(BINFOLDER)fshut.o
LIBUTF_OBJS=$(patsubst $(SBASE)/libutf/%.c,$(BINFOLDER)%.o,$(wildcard $(SBASE)/libutf/*.c))
GLUE_LS=$(BINFOLDER)dirent.o $(BINFOLDER)pwd_grp.o
# Programs built. Placement (see _image): init -> /nanos/core (PID 1); system utilities
# -> /nanos/bin; non-system apps (games/demos/tests) -> /apps.
USER_PROGS=init nsh cat ls sigtest fbtest timetest brktest inputtest fstest free usedll pipetest forkmany orphan ptytest nterm tuitest racetest envtest mmaptest mousetest doom nwm nwnote nwform rustform nwexp nwset nwterm nwabout
SYS_PROGS=nsh cat ls free nwm
APP_PROGS=sigtest fbtest timetest brktest inputtest fstest usedll pipetest forkmany orphan ptytest nterm tuitest racetest envtest mmaptest mousetest doom nwnote nwform rustform nwexp nwset nwterm nwabout
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
$(BINFOLDER)sigtest.nxe:   $(DYN_DEPS) $(BINFOLDER)sigtest.o
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
  $(BINFOLDER)dirent.o $(BINFOLDER)pwd_grp.o $(BINFOLDER)posixstubs.o
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
  -Iarch/include -Ikernel -Idrivers -Iinclude

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

KEXTS=kbd mouse
_kext: $(addprefix $(BINFOLDER),$(addsuffix .nkext,$(KEXTS)))

# Doom (doomgeneric). Old-C source needs -fcommon (GCC 10+ defaults to -fno-common, which
# breaks Doom's tentative globals) and warnings off; -DNORMALUNIX -DLINUX select the POSIX
# code paths; -lm for the renderer's trig/sqrt. Our platform layer (doomgeneric_nanos.c)
# replaces the shipped backends. Built as the `doom` program in USER_PROGS.
DOOM_DIR=user/third_party/doomgeneric
DOOM_CFLAGS=-ffreestanding -isystem $(PICOLIBC)/include -iquote kernel -Iuser -I$(DOOM_DIR) -D_DEFAULT_SOURCE -DNORMALUNIX -DLINUX -include user/libc-glue/compat-decls.h -include user/libc-glue/nx-dllimport.h -w -fcommon -fno-pic -fno-stack-protector
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
HINCLUDES=-Iarch/include -Ikernel -Idrivers -Ifs -Imm -Ilib -Iarch/x86/boot -Iarch/x86/mm -Ikext/mouse -Iuser/libnw -Iuser/nwm -Iuser/libnwui -Iuser/term
HOST_CXXFLAGS=-std=c++17 -O0 -g $(HINCLUDES) -Wall --coverage
TEST_BIN=/tmp/nanos_tests
TEST_SRCS=$(wildcard tests/*.cpp)
# Modules under test (grown as layers are added). Header-only modules contribute
# coverage via the .h patterns below.
TEST_MODULES=drivers/RamBlockDevice.cpp drivers/DeviceManager.cpp drivers/Console.cpp fs/Vfs.cpp fs/ExtFilesystem.cpp fs/SynthFs.cpp fs/RamFs.cpp fs/ext/Crc32c.cpp fs/ext/BlockCache.cpp fs/ext/ExtCsum.cpp fs/ext/ExtAllocator.cpp fs/ext/Journal.cpp kernel/Syscall.cpp kernel/NxeLoader.cpp kernel/KeyDecoder.cpp kernel/Scheduler.cpp kernel/Process.cpp kernel/Signal.cpp lib/String.cpp
TEST_MODULES+= arch/x86/boot/MultibootMmap.cpp mm/FrameAllocator.cpp mm/Heap.cpp arch/x86/mm/AddressSpace.cpp
TEST_MODULES+= drivers/Framebuffer.cpp drivers/Font8x16.cpp drivers/FbConsole.cpp drivers/Fbdev.cpp drivers/KeyboardDevice.cpp drivers/Pty.cpp
TEST_MODULES+= kext/mouse/MouseDevice.cpp   # MI half of the mouse kext (PS/2 decode -> evdev)
# NanWM (window server) pure cores — userland C, host-tested as C++ (g++ treats .c as C++).
# vtfont.c supplies the shared nx_font8x16 the gfx rasterizer draws with.
TEST_MODULES+= user/libnw/nwproto.c user/libnw/nw_gfx.c user/term/vtfont.c
TEST_MODULES+= user/nwm/nwm_core.c user/nwm/nw_compose.c
TEST_MODULES+= user/libnwui/nwui_core.c   # the pure UI-toolkit core (tree/layout/events)
TEST_MODULES+= user/term/vt.c             # the pure VT/ANSI terminal engine (shared by nterm/nwterm)
# lcov patterns selecting the modules whose coverage is gated (String is support).
COV_PATTERNS="*/RamBlockDevice.*" "*/DeviceManager.*" "*/Vfs.*" "*/ExtFilesystem.*" "*/Ext2Filesystem.*" "*/Ext4Filesystem.*" "*/ExtAllocator.*" "*/BlockCache.*" "*/ExtCsum.*" "*/Crc32c.*" "*/Journal.*" "*/SynthFs.*" "*/RamFs.*" "*/Syscall.*" "*/NxeLoader.*" "*/KeyDecoder.*" "*/Process.*" "*/Signal.*" "*/Framebuffer.*" "*/FbConsole.*" "*/Fbdev.*" "*/KeyboardDevice.*" "*/Pty.*" "*/MouseDevice.*" "*/MultibootMmap.*" "*/FrameAllocator.*" "*/Heap.*" "*/AddressSpace.*" "*/nwproto.*" "*/nw_gfx.*" "*/nwm_core.*" "*/nw_compose.*" "*/nwui_core.*" "*/vt.*"
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

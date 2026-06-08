# Architecture selection: machine-dependent knobs come from arch/$(ARCH)/arch.mk
# (CROSS, ARCH_VPATH, ARCH_INCLUDES, ARCH_LINKER, ARCH_SOURCES).
ARCH ?= x86
include arch/$(ARCH)/arch.mk

# Machine-independent objects (portable across architectures).
MI_SOURCES=kmain.o Kernel.o Console.o ExtFilesystem.o SynthFs.o RamBlockDevice.o DeviceManager.o Vfs.o
MI_SOURCES+= Syscall.o SyscallDispatch.o NxeLoader.o Exec.o FrameAllocator.o KeyDecoder.o Scheduler.o Process.o Signal.o
MI_SOURCES+= memory_manager.o List.o String.o icxxabi.o string_funcs.o
# Full link set = portable objects + the selected arch's machine-dependent objects.
SOURCES=$(MI_SOURCES) $(ARCH_SOURCES)

BINFOLDER=bin/
IMAGE_GRUB2=disk/image-grub2.img
# Partition starts at LBA 2048 (1MiB offset)
IMAGE_GRUB2_PART=$(IMAGE_GRUB2)?offset=1048576

DOCKER_IMAGE=nanos-build
DOCKER_RUN=docker run --rm --platform linux/amd64 -v $(CURDIR):/src -w /src $(DOCKER_IMAGE)

ifeq ($(wildcard /etc/nanos-build),)
# ============================================================================
# HOST side (macOS): build inside Docker, run QEMU natively.
# ============================================================================

docker-image:
	docker build --platform linux/amd64 -t $(DOCKER_IMAGE) docker/

build: docker-image
	$(DOCKER_RUN) make _all

image: docker-image
	$(DOCKER_RUN) make _image

iso: docker-image
	$(DOCKER_RUN) make _iso

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
VPATH=init:kernel:drivers:fs:mm:lib:$(ARCH_VPATH)
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

_image: _all _userland _grub2-image
	# System volume layout: everything NanOS lives under /nanos (core/bin/lib/kext/
	# config/cache/logs). GRUB stays in /boot. mkdir is idempotent across rebuilds.
	-printf "mkdir /nanos\nmkdir /nanos/core\nmkdir /nanos/bin\nmkdir /nanos/lib\nmkdir /nanos/kext\nmkdir /nanos/config\nmkdir /nanos/cache\nmkdir /nanos/logs\n" | debugfs -w "$(IMAGE_GRUB2_PART)" 2>/dev/null
	# Kernel + init (PID 1) in core; the rest of the programs in bin.
	printf "rm /nanos/core/kernel.bin\nwrite $(BINFOLDER)kernel.bin /nanos/core/kernel.bin\n" | debugfs -w "$(IMAGE_GRUB2_PART)"
	printf "rm /nanos/core/init.nxe\nwrite $(BINFOLDER)init.nxe /nanos/core/init.nxe\n" | debugfs -w "$(IMAGE_GRUB2_PART)"
	for p in nsh cat ls; do \
	  printf "rm /nanos/bin/$$p.nxe\nwrite $(BINFOLDER)$$p.nxe /nanos/bin/$$p.nxe\n" | debugfs -w "$(IMAGE_GRUB2_PART)"; \
	done

_iso: _all
	mkdir -p iso/boot/grub
	cp $(BINFOLDER)kernel.bin iso/boot/
	cp grub.cfg iso/boot/grub/
	grub-mkrescue -o nanos.iso iso/

_clean:
	-rm $(BINFOLDER)*.o $(BINFOLDER)*.d $(BINFOLDER)kernel.bin

# ----------------------------------------------------------------------------
# Userland: .nxe programs link against ported picolibc + our syscall glue (own
# linker script/base 0x400000, NOT in kernel SOURCES). picolibc headers come via
# -isystem; SyscallNr.h via -Ikernel. No <string.h> clash (lib/ is not on the path).
# ----------------------------------------------------------------------------
PICOLIBC=/opt/picolibc/i686-elf
SBASE=user/third_party/sbase
USER_CFLAGS=-ffreestanding -isystem $(PICOLIBC)/include -Ikernel -Iuser -Iuser/libc-glue/include -I$(SBASE) -D_DEFAULT_SOURCE -include user/libc-glue/compat-decls.h -Wall -fno-pic -fno-stack-protector
USER_LIBS=-L$(PICOLIBC)/lib -lc -lgcc
# Shared per-program objects: startup, .nxe header, the picolibc syscall glue, and
# the userland cwd layer (syscalls.o's path resolver lives in cwd.o).
USER_GLUE=$(BINFOLDER)crt0.o $(BINFOLDER)nxhdr.o $(BINFOLDER)syscalls.o $(BINFOLDER)cwd.o
# Per-program libutil subsets and the libutf objects (ls needs UTF + dir/pwd glue).
SBASE_UTIL_CAT=$(BINFOLDER)eprintf.o $(BINFOLDER)concat.o $(BINFOLDER)writeall.o
SBASE_UTIL_LS=$(BINFOLDER)eprintf.o $(BINFOLDER)ealloc.o $(BINFOLDER)reallocarray.o $(BINFOLDER)human.o $(BINFOLDER)fshut.o
LIBUTF_OBJS=$(patsubst $(SBASE)/libutf/%.c,$(BINFOLDER)%.o,$(wildcard $(SBASE)/libutf/*.c))
GLUE_LS=$(BINFOLDER)dirent.o $(BINFOLDER)pwd_grp.o
# Programs built (init -> /nanos/core, the rest -> /nanos/bin; see _image).
USER_PROGS=init nsh cat ls

# Link one program: $(call link_prog,<name>,<extra objects>)
define link_prog
	$(LD) -nostdlib -T user/nx.ld -o $(BINFOLDER)$(1).elf $(USER_GLUE) $(2) $(USER_LIBS)
	$(CROSS)objcopy -O binary $(BINFOLDER)$(1).elf $(BINFOLDER)$(1).nxe
endef

_userland: _userland-glue _userland-sbase
	$(CXX) $(USER_CFLAGS) -c user/init.c -o $(BINFOLDER)init.o
	$(call link_prog,init,$(BINFOLDER)init.o)
	$(CXX) $(USER_CFLAGS) -c user/nsh.c -o $(BINFOLDER)nsh.o
	$(call link_prog,nsh,$(BINFOLDER)nsh.o)
	$(CXX) $(USER_CFLAGS) -c $(SBASE)/cat.c -o $(BINFOLDER)cat.o
	$(call link_prog,cat,$(BINFOLDER)cat.o $(SBASE_UTIL_CAT))
	$(CXX) $(USER_CFLAGS) -c $(SBASE)/ls.c -o $(BINFOLDER)ls.o
	$(call link_prog,ls,$(BINFOLDER)ls.o $(SBASE_UTIL_LS) $(LIBUTF_OBJS) $(GLUE_LS))

# Build the shared startup/header/glue objects once.
_userland-glue:
	@mkdir -p $(BINFOLDER)
	nasm -f elf user/crt0.S -o $(BINFOLDER)crt0.o
	$(CXX) $(USER_CFLAGS) -c user/nxhdr.c -o $(BINFOLDER)nxhdr.o
	$(CXX) $(USER_CFLAGS) -c user/libc-glue/syscalls.c -o $(BINFOLDER)syscalls.o
	$(CXX) $(USER_CFLAGS) -c user/libc-glue/cwd.c -o $(BINFOLDER)cwd.o
	$(CXX) $(USER_CFLAGS) -c user/libc-glue/dirent.c -o $(BINFOLDER)dirent.o
	$(CXX) $(USER_CFLAGS) -c user/libc-glue/pwd_grp.c -o $(BINFOLDER)pwd_grp.o

# Build the vendored sbase libutil + libutf objects the coreutils link against.
_userland-sbase:
	for f in eprintf concat writeall ealloc reallocarray human fshut; do \
	  $(CXX) $(USER_CFLAGS) -c $(SBASE)/libutil/$$f.c -o $(BINFOLDER)$$f.o || exit 1; done
	for f in $(SBASE)/libutf/*.c; do \
	  $(CXX) $(USER_CFLAGS) -c $$f -o $(BINFOLDER)$$(basename $$f .c).o || exit 1; done

# ----------------------------------------------------------------------------
# Host-compiled test suite (doctest) + coverage gate.
# The storage stack is pure software, so we run it natively (host g++) against an
# in-memory RamBlockDevice. Memory/string come from libc (we do NOT link
# memory_manager/string_funcs); Console is stubbed by tests/host_shims.cpp.
# ----------------------------------------------------------------------------
HOST_CXX=g++
# Host include path: code dirs only, deliberately WITHOUT -Iinclude so that
# <string.h> resolves to libc (not the freestanding include/string.h).
HINCLUDES=-Iarch/include -Ikernel -Idrivers -Ifs -Imm -Ilib -Iarch/x86/boot -Iarch/x86/mm
HOST_CXXFLAGS=-std=c++17 -O0 -g $(HINCLUDES) -Wall --coverage
TEST_BIN=/tmp/nanos_tests
TEST_SRCS=$(wildcard tests/*.cpp)
# Modules under test (grown as layers are added). Header-only modules contribute
# coverage via the .h patterns below.
TEST_MODULES=drivers/RamBlockDevice.cpp drivers/DeviceManager.cpp drivers/Console.cpp fs/Vfs.cpp fs/ExtFilesystem.cpp fs/SynthFs.cpp kernel/Syscall.cpp kernel/NxeLoader.cpp kernel/KeyDecoder.cpp kernel/Scheduler.cpp kernel/Process.cpp kernel/Signal.cpp lib/String.cpp
TEST_MODULES+= arch/x86/boot/MultibootMmap.cpp mm/FrameAllocator.cpp arch/x86/mm/AddressSpace.cpp
# lcov patterns selecting the modules whose coverage is gated (String is support).
COV_PATTERNS="*/RamBlockDevice.*" "*/DeviceManager.*" "*/Vfs.*" "*/ExtFilesystem.*" "*/Ext2Filesystem.*" "*/Ext4Filesystem.*" "*/SynthFs.*" "*/Syscall.*" "*/NxeLoader.*" "*/KeyDecoder.*" "*/Process.*" "*/Signal.*" "*/MultibootMmap.*" "*/FrameAllocator.*" "*/AddressSpace.*"
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

SOURCES=loader.o kmain.o Kernel.o Console.o IOPort.o Gdt.o Idt.o irq.o isr.o
SOURCES+= Interrupt.o Keyboard.o ATA.o Hdd.o Ext2Filesystem.o
SOURCES+= memory_manager.o List.o String.o MultiTasking.o icxxabi.o string_funcs.o

BINFOLDER=bin/
IMAGE_GRUB2=fs/image-grub2.img
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

else
# ============================================================================
# CONTAINER side (Linux): real compilation, image and ISO creation.
# Tools resolved from the container PATH (i686-elf toolchain in /opt/cross).
# ============================================================================

CROSS?=i686-elf-
CXX=$(CROSS)gcc
AS=$(CROSS)as
LD=$(CROSS)gcc
CXXFLAGS=-ffreestanding -nostdlib -nostdinc++ -Iinclude -Wall --no-exceptions --no-rtti -fno-sized-deallocation -fno-leading-underscore
LDFLAGS=-Tlinker.ld -nostdlib -nostartfiles -lgcc
ASFLAGS=

_all: $(SOURCES) _link

_link:
	$(LD) $(LDFLAGS) -o $(BINFOLDER)kernel.bin $(foreach source,$(SOURCES),$(BINFOLDER)$(source))

# Build the GRUB2 ext2 skeleton once, then (re)write the kernel into it.
_grub2-image:
	./scripts/create-grub2-image.sh

_image: _all _grub2-image
	printf "rm /boot/kernel.bin\nwrite $(BINFOLDER)kernel.bin /boot/kernel.bin\n" | debugfs -w "$(IMAGE_GRUB2_PART)"

_iso: _all
	mkdir -p iso/boot/grub
	cp $(BINFOLDER)kernel.bin iso/boot/
	cp grub.cfg iso/boot/grub/
	grub-mkrescue -o nanos.iso iso/

_clean:
	-rm $(BINFOLDER)*.o $(BINFOLDER)kernel.bin

.s.o:
	$(AS) $(ASFLAGS) $< -o $(BINFOLDER)$@
.S.o:
	nasm -f elf $< -o $(BINFOLDER)$@
.cpp.o:
	$(CXX) -c $(CXXFLAGS) $< -o $(BINFOLDER)$@

# ----------------------------------------------------------------------------
# Host-compiled test suite (doctest) + coverage gate.
# The storage stack is pure software, so we run it natively (host g++) against an
# in-memory RamBlockDevice. Memory/string come from libc (we do NOT link
# memory_manager/string_funcs); Console is stubbed by tests/host_shims.cpp.
# ----------------------------------------------------------------------------
HOST_CXX=g++
HOST_CXXFLAGS=-std=c++17 -O0 -g -I. -Wall --coverage
TEST_BIN=/tmp/nanos_tests
TEST_SRCS=$(wildcard tests/*.cpp)
# Modules under test (grown as layers are added). Header-only modules contribute
# coverage via the .h patterns below.
TEST_MODULES=RamBlockDevice.cpp DeviceManager.cpp Vfs.cpp String.cpp
# lcov patterns selecting the modules whose coverage is gated (String is support).
COV_PATTERNS="*/RamBlockDevice.*" "*/DeviceManager.*" "*/Vfs.*"
COV_INFO=/tmp/cov.info
COV_MIN=90
# The repo is bind-mounted from a case-insensitive macOS FS, which makes
# `#include <string.h>` resolve to String.h (infinite recursion). Compile from a
# copy on the container's own case-sensitive FS so <string.h> means libc.
BUILDDIR=/tmp/nbuild

_test:
	@rm -rf $(BUILDDIR) && cp -a /src $(BUILDDIR)
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

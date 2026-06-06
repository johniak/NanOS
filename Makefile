SOURCES=loader.o kmain.o Kernel.o Console.o IOPort.o Idt.o irq.o isr.o
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

clean:
	$(DOCKER_RUN) make _clean
	-rm -rf iso/ nanos.iso $(IMAGE_GRUB2)

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

endif

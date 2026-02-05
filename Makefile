SOURCES=kmain.o Kernel.o loader.o Console.o IOPort.o Idt.o irq.o isr.o
SOURCES+= Interrupt.o Keyboard.o ATA.o Hdd.o Ext2Filesystem.o
SOURCES+= memory_manager.o List.o String.o MultiTasking.o icxxabi.o string_funcs.o

CXX=i686-elf-gcc
AS=i686-elf-as
LD=i686-elf-gcc
CXXFLAGS=-ffreestanding -nostdlib -nostdinc++ -Iinclude -Wall --no-exceptions --no-rtti -fno-sized-deallocation -fno-leading-underscore
LDFLAGS=-Tlinker.ld -nostdlib -nostartfiles -lgcc
BINFOLDER=bin/
ASFLAGS=
IMAGE=fs/image.img

all: $(SOURCES) link

clean:
	-rm $(BINFOLDER)*.o $(BINFOLDER)kernel.bin

link:
	$(LD) $(LDFLAGS) -o $(BINFOLDER)kernel.bin  $(foreach source,$(SOURCES),$(BINFOLDER)$(source))

install: all
	printf "rm /boot/kernel.bin\nwrite $(BINFOLDER)kernel.bin /boot/kernel.bin\n" | debugfs -w $(IMAGE)

run: install
	qemu-system-i386 -fda $(IMAGE)

.s.o:
	$(AS) $(ASFLAGS) $< -o $(BINFOLDER)$@
.S.o:
	nasm -f elf $< -o $(BINFOLDER)$@

.cpp.o:
	$(CXX) -c $(CXXFLAGS) $< -o $(BINFOLDER)$@

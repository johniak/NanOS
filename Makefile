SOURCES=kmain.o Kernel.o loader.o Console.o IOPort.o Idt.o irq.o isr.o Interrupt.o Keyboard.o

CXX=i586-elf-gcc
LD=i586-elf-gcc
CXXFLAGS=-nostdlib -nostdinc++ -Wall --no-exceptions --no-rtti 
LDFLAGS=-Tlinker.ld -nostdinc++ -nostartfiles
BINFOLDER=bin/
ASFLAGS=

all: $(SOURCES) link /mnt/osdrive/boot/kernel.bin

clean:
	-rm $(BINFOLDER)*.o $(BINFOLDER)kernel.bin

link:
	$(LD) $(LDFLAGS) -o $(BINFOLDER)kernel.bin  $(foreach source,$(SOURCES),$(BINFOLDER)$(source))

.s.o:
	i586-elf-as $(ASFLAGS) $< -o $(BINFOLDER)$@
.S.o:
	nasm -f elf $< -o $(BINFOLDER)$@

.cpp.o:
	$(CXX) -c $(CXXFLAGS) $< -o $(BINFOLDER)$@

/mnt/osdrive/boot/kernel.bin:bin/kernel.bin
	sudo cp bin/kernel.bin  /mnt/osdrive/boot/kernel.bin
	sync

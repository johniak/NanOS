# arch/x86/arch.mk — machine-dependent build knobs for i686 (x86, 32-bit).
#
# Included by the top-level Makefile as `arch/$(ARCH)/arch.mk`. Provides:
#   CROSS         cross-toolchain prefix
#   ARCH_VPATH    colon-separated dirs holding this arch's sources
#   ARCH_INCLUDES -I flags for this arch's headers
#   ARCH_LINKER   linker script for this arch
#   ARCH_SOURCES  this arch's machine-dependent objects (added to the link set)

CROSS ?= i686-elf-

# NOTE: during the MI/MD split the x86 sources still live in their original dirs;
# ARCH_VPATH/ARCH_INCLUDES point there and migrate to arch/x86/{boot,cpu,mm,drivers}
# step by step. ARCH_SOURCES already reflects the final MD partition.
ARCH_VPATH=arch
ARCH_INCLUDES=-Iarch
ARCH_LINKER=linker.ld

ARCH_SOURCES=loader.o Gdt.o Idt.o Interrupt.o IOPort.o irq.o isr.o nxjmp.o
ARCH_SOURCES+= MultibootMmap.o AddressSpace.o
ARCH_SOURCES+= Keyboard.o Hdd.o ATA.o AtaBlockDevice.o MultiTasking.o

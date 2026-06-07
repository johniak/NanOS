# arch/x86/arch.mk — machine-dependent build knobs for i686 (x86, 32-bit).
#
# Included by the top-level Makefile as `arch/$(ARCH)/arch.mk`. Provides:
#   CROSS         cross-toolchain prefix
#   ARCH_VPATH    colon-separated dirs holding this arch's sources
#   ARCH_INCLUDES -I flags for this arch's headers
#   ARCH_LINKER   linker script for this arch
#   ARCH_SOURCES  this arch's machine-dependent objects (added to the link set)

CROSS ?= i686-elf-

# The CPU/boot/asm sources now live under arch/x86/{boot,cpu,drivers}; the
# mm/multiboot and remaining PC-driver sources migrate here in later steps (until
# then they are found via the MI VPATH dirs: mm/, drivers/, kernel/).
ARCH_VPATH=arch/x86/boot:arch/x86/cpu:arch/x86/mm:arch/x86/drivers
ARCH_INCLUDES=-Iarch/x86/boot -Iarch/x86/cpu -Iarch/x86/mm -Iarch/x86/drivers
ARCH_LINKER=arch/x86/linker.ld

ARCH_SOURCES=loader.o Gdt.o Idt.o Interrupt.o IOPort.o irq.o isr.o nxjmp.o
ARCH_SOURCES+= cpu_x86.o irq_x86.o console_x86.o
ARCH_SOURCES+= bootinfo_x86.o MultibootMmap.o
ARCH_SOURCES+= mmu_x86.o AddressSpace.o
ARCH_SOURCES+= Keyboard.o Hdd.o ATA.o AtaBlockDevice.o MultiTasking.o

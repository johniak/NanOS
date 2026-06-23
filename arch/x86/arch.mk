# arch/x86/arch.mk — machine-dependent build knobs for i686 (x86, 32-bit).
#
# Included by the top-level Makefile as `arch/$(ARCH)/arch.mk`. Provides:
#   CROSS         cross-toolchain prefix
#   ARCH_VPATH    colon-separated dirs holding this arch's sources
#   ARCH_INCLUDES -I flags for this arch's headers
#   ARCH_LINKER   linker script for this arch
#   ARCH_SOURCES  this arch's machine-dependent objects (added to the link set)

CROSS ?= i686-elf-

# Build knobs shared with the top-level Makefile (mirrored in arch/x86_64/arch.mk).
ASM_FMT    ?= elf
QEMU       ?= qemu-system-i386
QEMU_CPU   ?= -cpu Nehalem
KARCHFLAGS ?=

# -Wconversion gate flags. Empty for i686 (kept green during the transition); the x86_64
# arch.mk turns the gate on (Plan 7) so LP64 truncations fail the build.
KWFLAGS ?=

# The CPU/boot/asm sources now live under arch/x86/{boot,cpu,drivers}; the
# mm/multiboot and remaining PC-driver sources migrate here in later steps (until
# then they are found via the MI VPATH dirs: mm/, drivers/, kernel/).
ARCH_VPATH=arch/x86/boot:arch/x86/cpu:arch/x86/mm:arch/x86/drivers:arch/x86/io
ARCH_INCLUDES=-Iarch/x86/boot -Iarch/x86/cpu -Iarch/x86/mm -Iarch/x86/drivers -Iarch/x86/io
ARCH_LINKER=arch/x86/linker.ld

ARCH_SOURCES=loader.o Gdt.o Idt.o Interrupt.o IOPort.o irq.o isr.o nxjmp.o
ARCH_SOURCES+= cpu_x86.o random_x86.o irq_x86.o syscall_x86.o fault_x86.o usermode_x86.o console_x86.o
ARCH_SOURCES+= bootinfo_x86.o MultibootMmap.o
ARCH_SOURCES+= mmu_x86.o AddressSpace.o
ARCH_SOURCES+= input_x86.o Hdd.o ATA.o AtaBlockDevice.o block_x86.o
ARCH_SOURCES+= switch.o sched_x86.o fork_x86.o
ARCH_SOURCES+= pci_x86.o
ARCH_SOURCES+= smp_x86.o

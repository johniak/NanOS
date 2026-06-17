# arch/x86_64/arch.mk — machine-dependent build knobs for x86_64 (long mode).
#
# Mirrors arch/x86/arch.mk. Selected by the top-level Makefile as arch/$(ARCH)/arch.mk
# when invoked with ARCH=x86_64. During the migration both arches coexist; this file
# is the single place x86_64 toolchain/format choices live.

CROSS ?= x86_64-elf-

# NASM object format and the QEMU binary for this arch (consumed by the Makefile).
ASM_FMT ?= elf64
QEMU     ?= qemu-system-x86_64
QEMU_CPU ?= -cpu qemu64

# Kernel-only codegen flags for x86_64: no SSE/MMX (kernel must not touch XMM across
# interrupts), no red zone (interrupt handlers would clobber it). Small code model is
# fine while the kernel lives low (@ 0x100000, < 2 GiB).
KARCHFLAGS ?= -mno-red-zone -mno-sse -mno-mmx -mno-80387

ARCH_VPATH=arch/x86_64/boot:arch/x86_64/cpu:arch/x86_64/mm:arch/x86_64/drivers:arch/x86_64/io
ARCH_INCLUDES=-Iarch/x86_64/boot -Iarch/x86_64/cpu -Iarch/x86_64/mm -Iarch/x86_64/drivers -Iarch/x86_64/io
ARCH_LINKER=arch/x86_64/linker.ld

# Plan 1 bring-up: only the boot trampoline + a 64-bit C entry stub. The full MD set
# (Gdt/Idt/interrupts/paging/drivers) is filled in by later plans.
# Plan 3 adds the paging layer (AddressSpace + the <arch/mmu.h> impl). loader.o/entry64.o stay
# from Plan 1; Plan 2's console/multiboot objects (if already present) are listed by that plan.
ARCH_SOURCES=loader.o entry64.o AddressSpace.o mmu_x86_64.o

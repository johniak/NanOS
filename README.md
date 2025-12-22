NanOS
=====

Tiny x86 operating system written in C++ (object-oriented) with a bit of
assembly. This repository is an **academic exploration** of how booting,
kernel initialization, interrupts, basic device I/O, and a filesystem can
work at a low level. It is intentionally minimal and is **not** intended
for production or real-world use.

Academic purpose
----------------
This project focuses on learning and understanding:
- how a Multiboot/GRUB bootloader hands control to a kernel
- how a kernel sets up an IDT and handles interrupts
- how basic drivers (VGA text console, keyboard, ATA PIO) work
- how a simple ext2 reader can locate and read files

Features
--------
- Ext2 filesystem support (*read-only*):
  - list directory entries
  - read files by inode or path
- Raw ATA PIO communication:
  - sector read
  - sector write
- VGA text console:
  - screen output
  - keyboard input
  - scrolling, cursor movement, clear screen
- Interrupt handling
- Basic PIT timer handling
- Simple `String` and `List` classes
- Very basic memory manager (bump allocator)
- C++ operators `new`/`delete` support

Requirements
------------
- `i586-elf-gcc` cross-toolchain
- `nasm`
- `make`
- `qemu-system-i386`
- Linux host for loopback mounting (see "Disk image" below)

Build
-----
The Makefile builds `bin/kernel.bin` and copies it to a mounted ext2 image
at `/mnt/osdrive/boot/kernel.bin`.

```
make
```

Run (QEMU)
----------
There are prebuilt images in `fs/`. The simplest run command is:

```
qemu-system-i386 -fda fs/image.img
```

Disk image (Linux only)
-----------------------
To mount the ext2 image and update the kernel:

```
sudo mkdir -p /mnt/osdrive
sudo losetup /dev/loop0 fs/image.img
sudo mount -t ext2 /dev/loop0 /mnt/osdrive
make
```

Unmount when done:

```
sudo umount /mnt/osdrive
sudo losetup -d /dev/loop0
```

If you need to rebuild a bootable image from scratch, see `fs/readme` for
the GRUB + floppy image workflow.

Project layout (high level)
---------------------------
- `loader.s`: Multiboot header and entry stub
- `kmain.cpp`: kernel entry point
- `Idt.*`, `Interrupt.*`: IDT setup and ISR/IRQ dispatch
- `Console.*`: VGA text-mode console
- `Keyboard.*`: keyboard IRQ handler
- `ATA.S`, `Hdd.*`: ATA PIO disk access
- `Ext2Filesystem.*`: ext2 reader (read-only)
- `MultiTasking.*`: experimental scheduling
- `memory_manager.*`: minimal allocator
- `String.*`, `List.*`: utility types
- `fs/`: disk images and GRUB stages

Notes and limitations
---------------------
- The memory manager is extremely simple and not safe for real use.
- The ext2 implementation is incomplete and only handles basic cases.
- Multitasking is experimental and not a full context switch.
- This is purely educational/academic: use it to understand "how it works"
  rather than to build a production OS.

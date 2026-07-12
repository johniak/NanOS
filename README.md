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
  - keyboard input (echo only, no shell)
  - scrolling, cursor movement, clear screen
- Interrupt handling (IDT, ISR, IRQ)
- Basic PIT timer handling
- Simple `String` and `List` classes
- Very basic memory manager (bump allocator)
- C++ operators `new`/`delete` support

Requirements
------------
- `i686-elf-gcc` cross-toolchain
- `nasm`
- `make`
- `qemu-system-i386`
- `e2fsprogs` (for `debugfs`)

### macOS (Homebrew)

```bash
brew install i686-elf-gcc nasm e2fsprogs qemu
```

Note: `e2fsprogs` is keg-only. Add to PATH:
```bash
export PATH="/opt/homebrew/opt/e2fsprogs/sbin:/opt/homebrew/opt/e2fsprogs/bin:$PATH"
```

### Linux (Debian/Ubuntu)

```bash
sudo apt install gcc-i686-linux-gnu nasm e2fsprogs qemu-system-x86
```

Build
-----

```bash
make          # build kernel only
make install  # build and install kernel into fs/image.img
make clean    # remove build artifacts
```

The Makefile builds `bin/kernel.bin` and uses `debugfs` to write it into
the ext2 image at `/boot/kernel.bin`.

Run (QEMU)
----------

The kernel (~1.6MB) is larger than the floppy image (~1.5MB), so use QEMU's
direct kernel loading:

```bash
qemu-system-i386 -kernel bin/kernel.bin -hda fs/image.img
```

Or build and run in one step (requires updating Makefile `run` target):

```bash
make && qemu-system-i386 -kernel bin/kernel.bin -hda fs/image.img
```

### What you'll see

```
NanoOS initialize...
```

The keyboard works (type and characters appear), but there is no shell or
command interpreter yet.

Boot sequence
-------------
1. **QEMU Multiboot** loads `kernel.bin` at 0x1000000
2. **`loader.s`** sets up stack, saves Multiboot magic, calls `kmain()`
3. **`kmain.cpp`** verifies Multiboot magic, creates `Kernel` instance
4. **`Kernel::start()`** initializes:
   - VGA console (clear screen, print "NanoOS initialize...")
   - IDT (interrupt descriptor table)
   - Keyboard driver (IRQ1)
   - Ext2 filesystem (reads from disk at LBA 2048)
   - MultiTasking (experimental, tries to load `/init.bin`)
5. **Main loop** runs indefinitely

Project layout
--------------
```
├── loader.s          # Multiboot header and entry stub
├── kmain.cpp         # kernel entry point
├── Kernel.cpp        # main kernel initialization
├── Idt.cpp           # IDT setup
├── Interrupt.cpp     # ISR/IRQ dispatch
├── isr.S, irq.S      # low-level interrupt stubs (NASM)
├── Console.cpp       # VGA text-mode console
├── Keyboard.cpp      # keyboard IRQ handler
├── ATA.S, Hdd.cpp    # ATA PIO disk access
├── Ext2Filesystem.h  # ext2 reader (read-only)
├── MultiTasking.cpp  # experimental scheduling
├── memory_manager.cpp# minimal bump allocator
├── String.cpp        # string utility class
├── List.h            # template dynamic array
├── linker.ld         # linker script (kernel at 0x1000000)
├── fs/
│   ├── image.img     # ext2 disk image with GRUB
│   └── readme        # image creation notes
└── include/          # header files
```

Disk image
----------

The `fs/image.img` is a small ext2 filesystem with GRUB Legacy installed.
To update the kernel in the image:

```bash
# Using debugfs (works on macOS and Linux, no root required)
printf "rm /boot/kernel.bin\nwrite bin/kernel.bin /boot/kernel.bin\n" | debugfs -w fs/image.img
```

To list contents:
```bash
debugfs -R "ls /boot" fs/image.img
```

If you need to rebuild a bootable image from scratch, see `fs/readme` for
the GRUB + floppy image workflow.

Notes and limitations
---------------------
- **No shell**: keyboard input is echoed but there's no command interpreter
- The memory manager is extremely simple (bump allocator, `free()` is a no-op)
- The ext2 implementation only handles direct block pointers (small files)
- Multitasking is experimental and incomplete
- Kernel is too large for floppy boot; use QEMU `-kernel` option
- This is purely educational: use it to understand "how it works"
  rather than to build a production OS

Ecosystem & reproducibility
---------------------------
Everything NanOS-related — this repo, the cross-SDK, and every ported
upstream project (openssl, dropbear, git, vim, mesa, the QEMU/virglrenderer
host GL forks, …) — lives in the [NanOS-labs](https://github.com/NanOS-labs)
GitHub organization, each fork carrying its own port recipe. A fresh machine
rebuilds the whole OS with `nanos-sdk/scripts/bootstrap.sh`; the full repo
map, dependency order and walkthrough are in [docs/ECOSYSTEM.md](docs/ECOSYSTEM.md).

License
-------
Academic/educational use.

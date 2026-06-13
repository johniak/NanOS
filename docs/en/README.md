# NanOS Documentation (English)

Architecture references for the NanOS kernel and userland. Polish version: [`../pl/`](../pl/README.md).

| Doc | What it covers |
|---|---|
| [boot.md](boot.md) | Boot sequence: GRUB2/Multiboot → `loader.s` → `kmain` → `Kernel::start`, GDT/IDT/PIC, the triple-fault fix, handoff to `init`. |
| [memory.md](memory.md) | Frame allocator, paging, per-process address spaces, the kernel heap, the full memory map, `brk`/`mmap`. |
| [scheduler.md](scheduler.md) | Deferred-preemption scheduler, tasks vs processes, context switch, blocking/wakeups, fork/exec/wait, signals. |
| [syscalls.md](syscalls.md) | The `int 0x80` Linux-i386 ABI, the `Syscalls` core, the full dispatch table, errno, stubs. |
| [filesystem.md](filesystem.md) | The VFS namespace, the on-disk layout, ext2/ext4 read+write + JBD2, links, the storage stack. |
| [nxe-ndl.md](nxe-ndl.md) | The `.nxe`/`.ndl` format, the loaders (mknx/NxeLoader/DynLoader), dynamic linking, the ring-3 process lifecycle. |
| [kext.md](kext.md) | Loadable kernel modules (`.nkext`): format, loader, the kernel↔kext ABI, the existing drivers. |
| [pci.md](pci.md) | The PCI bus: enumeration, BAR decode, the MI/MD config-space backend, the driver discovery hook (e1000). |
| [terminal.md](terminal.md) | Console (VGA→fbcon), the evdev keyboard, the PTY + line discipline + job control, the shared VT engine, `nterm`. |
| [ipc-time.md](ipc-time.md) | Pipes (`pipe(2)` ring FIFO) and the time subsystem (RTC boot epoch + monotonic tick, `clock_gettime`, uptime). |
| [lib.md](lib.md) | The freestanding utility layer: `<string.h>`, the `String`/`List` containers, and the C++ ABI glue. |
| [networking.md](networking.md) | The full IPv4 stack: e1000 → Ethernet/ARP → IP → ICMP/UDP/RAW/TCP → BSD sockets → the Linux socket ABI, DHCP, `/proc/net`. |
| [windowing.md](windowing.md) | NanWM: the `nwm` compositor, the client↔compositor protocol, `libnw`, the `libnwui` toolkit, the GUI apps. |
| [writing-apps.md](writing-apps.md) | How-to: writing an in-tree app, GUI apps (NanWM), and porting real Linux programs with the nanos-sdk. |

Design specs and plans live under [`../superpowers/`](../superpowers/).

---
name: nanos-dual-boot-limine
description: Dual-firmware boot (BIOS+UEFI) via Limine on a hybrid GPT image; NanOS now BOOTS ON THE REAL Dell Latitude 5310 from USB (UEFI→Limine→kernel→init→shell) after 5 real-HW fixes
metadata: 
  node_type: memory
  type: project
  originSessionId: 661d85b4-63ba-4deb-8adc-c850a1d3125e
---

NanOS x86_64 now boots to a shell under **both legacy BIOS and UEFI** from one hybrid **GPT** image,
via the **Limine** bootloader. Spec `docs/superpowers/specs/2026-06-19-dual-boot-uefi-bios-design.md`,
plan `docs/superpowers/plans/2026-06-19-dual-boot-uefi-bios.md`. Branch `feat/boot-uefi-bios` (off
`develop`). **All 5 plan tasks done; `make verify64` green** (635 host tests + BIOS + UEFI + live-USB
smokes). This was **piece 1 of 3** toward "boot NanOS on the Dell Latitude 5310 from a USB stick";
**pieces 2 and 3 are now DONE too** (branch `feat/real-hw-boot`, all on top): piece 3 = >1GiB memory
map (huge-page kernel identity map + per-process user-VA-window privatization; `make smoke-bigmem` -m
6144 in verify64 — see [[nanos-x64-paging-carryforward]]); piece 2 = xHCI BIOS/SMM handoff (commit
66de6b1: `biosHandoff()` USB Legacy Support OS-ownership claim + SMI silence, + guarded Intel
vendor=0x8086 port routing XUSB2PR/USB3_PSSEN, in `arch/x86_64/drivers/xhci_x86_64.cpp` before
`controllerInit`). Both xHCI additions are NO-OPS on QEMU (no Legacy cap, non-Intel vendor) so CI bar =
"USB smokes stay green"; **real validation = the Dell itself, not yet run on hardware.** So all three
QEMU-side gaps toward Dell USB boot are closed; remaining is booting on the actual machine (plus NVMe /
I219 NIC, which are separate roadmap items).

**What shipped:**
- Limine in the `nanos-build` Docker image (trailing layer): `v8.x-binary` (8.7.0) — prebuilt
  `BOOTX64.EFI` + `limine-bios.sys` + the host `limine` deploy tool (`make`). Fetched via wget+tar
  (no git); tarball top dir is `Limine-8.x-binary` (capital L, no `v`) so it's derived dynamically.
- Hybrid GPT image (`scripts/create-grub2-image.sh`, gated by `NANOS_BOOT=limine` so the frozen i686
  GRUB/MBR path is untouched): P1 bios_boot(1MiB) + P2 ESP/FAT16(32MiB) + P3 ext2 root(label NANOS).
  No loop device: parted + mtools (ESP built in a separate file, dd'd in) + mke2fs -E offset +
  `limine bios-install <img> 1`. `limine.conf` (on ESP, `protocol: multiboot1`,
  `path: fslabel(NANOS):/nanos/core/kernel.bin`). Kernel Multiboot1 UNCHANGED.
- Kernel: MI `kernel/PartitionTable.cpp` `firstFsPartitionLba()` — MBR + GPT partition discovery
  (protective-MBR → GPT header → walk entries → probe ext magic). Host-tested
  (`tests/test_partitiontable.cpp`). `Kernel.cpp` firstPartitionLba delegates to it. Covers ATA + USB.
- `make smoke-uefi` (scripts/smoke-uefi.sh, OVMF/edk2 from Homebrew qemu `edk2-x86_64-code.fd`) added
  to `verify64` alongside `smoke-x86_64` (BIOS) and `smoke-usb`.

**Gotchas hit + fixed (remember for real HW / future bootloader work):**
1. Docker build context is `docker/` (not repo root) — `docker build -t nanos-build docker/`.
2. GPT BIOS boot via Limine REQUIRES a bios_boot partition AND `limine-bios.sys` copied onto a
   readable FS (Limine searches root//boot//limine of a partition) — installing into bios_boot alone
   is not enough (Limine prints a "copy limine-bios.sys" reminder).
3. mke2fs must NOT fill to the disk end or it overwrites the **backup GPT header** ("Secondary header
   not valid"). End the root partition before the disk end (used 318MiB of 320MiB) and size mke2fs to
   match exactly.
4. **Limine's ext4 reader panics "block longer than extent"** on the extent tree debugfs writes →
   the root is **ext2** (block-mapped, no extents); the kernel auto-detects ext2 and mounts it
   read-write. TRADEOFF: ext2 has no JBD2 journal (less crash-safe than the ext4 the USB work used).
   Future option to restore ext4+JBD2: put only kernel.bin on the FAT ESP (Limine reads FAT fine) and
   keep the root ext4, so Limine never reads ext4.
5. `limine.conf` `serial: yes` sends Limine's own diagnostics to COM1 — essential for headless debug
   (its errors otherwise go to the VGA screen, invisible under `-display none`).
6. UEFI/OVMF first boot is slower (dropbear ed25519 keygen) — the smoke must wait for `bash-5`
   specifically, NOT match "starting shell/services" (which prints before init runs).

**REAL-HARDWARE BOOT ACHIEVED (Dell Latitude 5310, UEFI Class 3, Comet Lake, NVMe+xHCI):** NanOS
boots from a USB stick all the way to the init→shell on the actual machine. Found+fixed five
real-HW-only bugs that QEMU/OVMF had hidden (all on `feat/real-hw-boot`-ish work in session
661d85b4; debug scaffolding since removed — kept `arch::debugBar` FB POST-code primitive as a
bring-up aid):
1. **ESP must be FAT32, not FAT16.** mtools picked FAT16 at 32 MiB → Dell firmware silently
   reset instead of reading the ESP (OVMF tolerated FAT16). Fix: 64 MiB ESP, `mformat -F`
   (FAT32); root moved 34→66 MiB (`ROOT_OFFSET`=69206016, Makefile `IMAGE64_GRUB2_PART` offset too).
2. **Loader identity map too small.** `loader.S` mapped only 1 GiB; on 8-16 GiB HW the Multiboot
   info / E820 / framebuffer sit higher and a pre-`initPaging` access faulted. Fix: map [0,64 GiB)
   with 2 MiB pages (64 contiguous PDs; no pdpe1gb so QEMU-safe).
3. **MMIO physical address truncated to 32-bit.** `mmuMapKernelMmio`/`mmuMapUserFb`/`FbInfo.phys`
   were `uint32_t`; real-HW LFB/BARs sit >4 GiB. Widened the whole framebuffer phys path to 64-bit.
4. **THE big one — 4 KiB MMIO mapped onto a live 2 MiB huge page.** `mmuInitKernel` huge-maps
   [0,topOfRam) then `mapRange`d the LAPIC (0xFEE00000) + framebuffer 4 KiB. On QEMU (512 MiB)
   those MMIO addrs were ABOVE the map (fresh tables, fine); on the Dell (16 GiB) they're INSIDE it,
   so `mapRange` walked the huge PDE as a page-table ptr → corruption → triple fault right after the
   WHITE POST bar. Fix: `g_identityTop` guard — skip re-mapping MMIO already covered by the huge map.
5. **USB-MSC multi-sector reads return only the first 512 B on real xHCI.** Enumeration+mount worked
   (sector 0 read OK) but ext dir blocks (1 KiB = 2 sectors) read back half-zero → root parsed as
   just `.`/`..`, init "not found". Fix: ONE READ(10)/WRITE(10) per sector in
   `UsbMscBlockDevice` (mirrors the old ATA multi-sector-desync fix). ALSO needed: xHCI ports start
   unpowered on real HW — set Port Power (PP) on every port + poll for connect debounce before
   `usbEnumerateAll` (QEMU powered ports + set CCS instantly, hiding it).

Remaining for daily-driver: NVMe (Dell's real disk), I219 NIC on metal (see [[nanos-nic-e1000e-i219]]),
internal display native res (got 1024x768 GOP), battery/ACPI, suspend. Multi-sector USB read is
per-sector (slow) — a proper multi-TRB bulk path in `xhciSubmit` is a perf follow-up.

See [[nanos-usb-stack]] (live-USB root), [[nanos-x64-migration]], [[nanos-real-hardware-roadmap]]
(Dell Latitude 5310 target).

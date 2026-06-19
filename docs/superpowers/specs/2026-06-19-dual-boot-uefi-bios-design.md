# Dual-firmware boot (UEFI + BIOS) via Limine — design

**Date:** 2026-06-19
**Branch:** `feat/boot-uefi-bios` (off `develop`; x86_64 is the sole development arch, i686 frozen).
**Status:** design approved in shape, ready for the implementation plan.

---

## 1. Goal & scope

Make NanOS boot **from a single GPT USB stick under both UEFI and legacy BIOS**, using the **Limine**
bootloader, with the existing Multiboot1 kernel **unchanged**. The stick carries a **writable ext4
root** (the live-USB system). Verified end-to-end in QEMU under **both** firmware paths (SeaBIOS for
BIOS, OVMF/edk2 for UEFI) before touching real hardware.

This is **piece 1 of 3** of "NanOS boots to a shell on the Dell Latitude 5310 from a USB stick":
- **Piece 1 (this spec):** dual-firmware boot + GPT image + Limine. QEMU-verifiable in full.
- **Piece 2 (out of scope):** xHCI real-hardware bring-up — BIOS/SMM handoff (USBLEGSUP) + Intel port
  routing — so the kernel reads the USB root after `ExitBootServices`. QEMU doesn't need it.
- **Piece 3 (out of scope):** real-RAM memory map + paging window (the Dell has 8–16 GB; the kernel
  currently maps a ~1 GiB identity window). QEMU hides it with `-m 512`.

Pieces 2 and 3 are the remaining "real-silicon vs QEMU" gaps; piece 1 deliberately isolates the
bootloader/image problem so it is fully testable in emulation.

**Why Limine (decided):** purpose-built for hobby OSes; ships **prebuilt** BIOS stages + `BOOTX64.EFI`
(no `grub-mkimage`, no `grub-efi-amd64-bin`); its deploy tool installs the BIOS boot record **on a flat
image file without a loop device** (our unprivileged Docker build cannot use `grub-install`); reads
ext4; and supports the **Multiboot1** protocol, so our kernel needs no changes. The current GRUB path
is BIOS-only and the dual-GRUB hybrid would be a fragile hand-rolled `grub-mkimage ×2` + offset dance.

## 2. Disk image layout (GPT)

```
GPT + protective MBR
 ├─ P1  bios_boot  type EF02, 1 MiB        -> `limine bios-install <img> 1` embeds the BIOS stages here
 ├─ P2  ESP        type EF00, FAT16, 32 MiB -> /EFI/BOOT/BOOTX64.EFI + /EFI/BOOT/limine.conf (UEFI + BIOS read it)
 └─ P3  root       type 8300, ext4, label "NANOS", remainder
                                            -> the full NanOS system: /nanos (incl. core/kernel.bin), /apps
```

A **dedicated BIOS-boot partition (P1)** is required for BIOS booting from a GPT disk — `limine
bios-install <image> 1` embeds Limine's BIOS stages into partition 1 (this is the documented Limine GPT
flow; there is no `limine-bios.sys` file to place). The **ESP (P2)** holds the UEFI binary
`BOOTX64.EFI` and `limine.conf`; Limine reads FAT, so the BIOS path finds the same `limine.conf` there
too. The **kernel and the whole system stay on the ext4 root (P3)** — exactly where `_image64` already
writes them — and `limine.conf` references the kernel by filesystem label, so nothing is duplicated.

The ESP is a **fixed 32 MiB**, so partition offsets are deterministic (parted aligns to 1 MiB):
P1 = 1 MiB, P2 = 2 MiB, **P3 (root) = 34 MiB = byte offset 35651584** — a constant the Makefile uses for
its `debugfs` populate step (replacing today's hard-coded `?offset=1048576`).

## 3. Bootloader (Limine)

- **BIOS:** firmware runs Limine's BIOS stage embedded in P1 by `limine bios-install <image> 1`; it
  searches partitions for `limine.conf` (found on the FAT ESP) and loads the kernel.
- **UEFI:** firmware auto-runs `EFI/BOOT/BOOTX64.EFI` (the removable-media default path), which reads
  `/EFI/BOOT/limine.conf` from the same ESP.
- **`limine.conf`** (on the ESP) — one Multiboot1 entry pointing at the kernel on the ext4 root by
  label (verified against Limine docs; `path:` + `protocol:` are the real keys, `fslabel(...)` resolves
  the root partition):
  ```
  timeout: 0
  /NanOS
      protocol: multiboot1
      path: fslabel(NANOS):/nanos/core/kernel.bin
  ```
  Per the Limine docs, the multiboot1 info it passes includes `mem_*`, `mmap_length/addr`, and
  `fb_addr/pitch/width/height/bpp` — exactly the fields `bootinfo_x86_64.cpp` reads (it was written
  against GRUB's Multiboot1). The kernel (Multiboot1, loaded at 1 MiB, 32→64 trampoline) is **not
  modified** by this piece.

## 4. Kernel change: GPT partition discovery (MI, host-tested)

The image is now GPT, so sector 0 is a **protective MBR** (a single type-`0xEE` entry). The existing
`firstPartitionLba()` (in `kernel/Kernel.cpp`) only understands MBR partition entries and would mis-read
this. Add **GPT parsing**:

- Read sector 0. If it is a protective MBR (an entry of type `0xEE`), read the **GPT header** (LBA 1),
  validate its signature `"EFI PART"`, then walk the **partition entry array** (at the header's
  `PartitionEntryLBA`, `NumberOfPartitionEntries` × `SizeOfPartitionEntry`).
- For each entry with a non-zero type GUID, probe the ext superblock magic (`0xEF53` at offset 1080 of
  the partition) at its `FirstLBA`; return the `FirstLBA` of the first partition that probes as ext.
- If sector 0 is a classic MBR (no `0xEE`), keep today's MBR scan. Fallback LBA 2048 as today.

This runs for **both** the ATA `-drive` path and the USB-MSC root, so one change covers both. It is pure
MI logic over a `BlockDevice`, host-tested against a crafted GPT fixture image (a small RAM disk with a
protective MBR + GPT header + one ext partition), added to the doctest suite (`tests/test_*` +
`TEST_MODULES`/`COV_PATTERNS`). `make check-arch` stays clean.

## 5. Build changes

- **Docker `nanos-build`** — add, in a **trailing layer** (so the cross-toolchain cache is untouched, as
  with `lcov`/`grub-pc-bin`): a pinned **Limine binary release** (provides `limine-bios.sys` +
  `BOOTX64.EFI`) and the built `limine` host deploy utility (`make` in the release tree). `mtools`
  (already present) builds the FAT ESP with no mount/loop; `parted` (present) makes the GPT.
- **`scripts/create-grub2-image.sh` → `scripts/create-image.sh`** — rewrite the skeleton builder for
  GPT + Limine, still **no loop device / no privileges**:
  1. `dd` a blank image; `parted -s mklabel gpt`; create P1 (bios_boot, 1–2 MiB, `set 1 bios_grub on`),
     P2 (ESP, 2–34 MiB, `set 2 esp on`), P3 (root, 34 MiB–100%).
  2. Build a 32 MiB FAT ESP in a **separate file** with `mformat`/`mmd`/`mcopy` (BOOTX64.EFI +
     EFI/BOOT/limine.conf), then `dd` it into the image at the P2 offset (2 MiB).
  3. `mke2fs -t ext4 -L NANOS -E offset=35651584` for the root at the P3 offset (34 MiB).
  4. `limine bios-install <image> 1` to embed the BIOS stages into the bios_boot partition.
- **`_image64`** (Makefile) — change the `IMAGE64_GRUB2_PART` offset from `1048576` to **`35651584`**
  (the fixed P3 offset); all `debugfs` populate steps then target the ext4 root unchanged. The image
  filename (`disk/image64-grub2.img`) and the `smoke-*` paths stay as-is.

The i686 image path (`make image`) is **out of scope** here (i686 is frozen); this piece touches the
x86_64 image build only. If the shared script needs to keep producing the old i686 image, it stays
behind an `ARCH`/`IMAGE_PATH` switch, with the GPT+Limine path used for x86_64.

## 6. Verification (QEMU, both firmwares)

- **BIOS** — `make smoke-x86_64` (existing gate) on the new GPT image; QEMU's default SeaBIOS boots the
  Limine BIOS path → asserts the shell is reached + zero faults.
- **UEFI** — new `make smoke-uefi` / `scripts/smoke-uefi.sh`: boot the host QEMU with the edk2/OVMF
  firmware (`-drive if=pflash,format=raw,readonly=on,file=<edk2-x86_64-code.fd>` from the Homebrew qemu
  install, plus a writable vars copy) + the same image → UEFI runs `BOOTX64.EFI` → Limine → kernel →
  shell, zero faults. This exercises the **GOP framebuffer** and the **Multiboot1 handoff from Limine**.
- Wire both into `verify64`: `verify64: test64 smoke-x86_64 smoke-uefi smoke-usb`.

## 7. Risks & verification points

- **Limine → `bootinfo_x86_64` Multiboot1 handoff.** The integration risk: does Limine's MB1 info carry
  the framebuffer + memory map the way our `bootinfo_x86_64.cpp` expects (it was written against GRUB)?
  First thing checked in QEMU (both firmwares); the framebuffer console showing boot text is the signal.
- **GOP framebuffer at a high physical address.** OVMF often places the GOP framebuffer near
  `0x80000000`, above the kernel's ~1 GiB identity window. `mmuMapKernelMmio` maps it explicitly, so the
  console should work; if it faults under OVMF, that is an early signal for piece 3 (memory map) and we
  address the explicit-MMIO-map path then.
- **OVMF on the macOS host.** Need the path to `edk2-x86_64-code.fd` (ships with Homebrew qemu) + a
  writable vars file; the smoke script locates it (with a clear error if absent).
- **Limine version drift.** Pin a specific Limine release in the Dockerfile so the prebuilt binaries and
  the `limine.conf` schema are stable across rebuilds.
- **GPT build arithmetic without a loop device.** Mitigated by letting `parted` lay out partitions,
  `mtools` build the FAT, and `mke2fs -E offset` place ext4 — the same no-mount toolkit the current
  script already relies on.

## 8. Out of scope (explicit)

- Reading the USB root on **real** xHCI hardware (piece 2: USBLEGSUP handoff + Intel port routing).
- Real multi-GB **memory map / paging window** (piece 3).
- NVMe / internal-disk boot (the live-USB stick is the target medium).
- i686 image changes (frozen arch).
- Secure Boot (the Dell's firmware setting; out of scope — assume it is disabled for a custom OS).

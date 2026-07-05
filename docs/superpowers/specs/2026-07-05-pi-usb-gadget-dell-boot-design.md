# pendrak — network-attached USB boot device for the Dell

**Date:** 2026-07-05
**Status:** design approved, pending spec review → implementation plan
**Topic:** Replace the physical pendrive swap in the Dell i915 dev loop with a Raspberry Pi Zero W that presents the NanOS disk image to the Dell as a USB mass-storage gadget, updated over WiFi from the MacBook via `scp`/`rsync`.

## Problem

The current Dell inner loop (`make update-dell`, `make flash-dell`) writes the NanOS
image (or just `kernel.bin` + `i915.nkext`) onto a physical Kingston USB stick with
macOS `debugfs` on the raw partition. Every iteration requires physically moving the
stick from the MacBook to the Dell and back. We want to eliminate the physical swap:
the MacBook should push updates over the network and the Dell should boot the result
directly, with no cable-shuffling between iterations.

## Solution overview

A Raspberry Pi Zero W (`pi@pendrak.local`) sits between the two:

- **Always on, own power** (charger/powerbank on the PWR micro-USB port). Its OTG data
  port (micro-USB → USB-A) goes to a Dell USB port carrying **data only**. This is
  required so the gadget is enumerated *before* the Dell firmware scans USB at POST — a
  Pi powered from the Dell would boot too late (~20-30 s) to be a boot device.
- **WiFi** on the LAN (that is how `pendrak.local` resolves; the Zero W has on-board
  WiFi, and WiFi is independent of the USB OTG port).
- Runs a **configfs USB mass-storage gadget** whose backing file is the full NanOS disk
  image `~/nanos/image64.img`. The Dell sees a normal removable USB disk and boots it.

The MacBook keeps building as today and pushes changes over WiFi. Two push paths mirror
the existing `flash-dell` / `update-dell` split.

### Environment facts (probed 2026-07-05)

- Pi: **Raspberry Pi Zero W Rev 1.1** (BCM2835, armv6l, dwc2 OTG, on-board WiFi).
- OS: **Raspbian trixie (Debian 13)**, kernel **6.18.34** — modern configfs gadget
  support (`libcomposite`, `usb_f_mass_storage`).
- `/boot/firmware/config.txt` currently: `otg_mode=1` and `dtoverlay=dwc2,dr_mode=host`
  — the port is in **host** mode; must switch to device (`dr_mode=peripheral`) for the
  gadget. One reboot required.
- 51 GB free on `/` — ample for the ~320 MiB image plus working copies.

## Why not rsync-on-the-full-image / why surgery

`make image64` calls `scripts/create-image.sh`, which runs a **fresh `mkfs`** every build
(new UUID/timestamps; `debugfs write` stamps mtime = now). Two consecutive
`image64.img` therefore differ block-wise almost everywhere even when file *contents* are
identical. So an `rsync` delta of the whole image would be large — it cannot replace the
surgical two-file write. The fast loop must send only the files that changed
(`kernel.bin` + `i915.nkext`, ~6 MiB) and write them into the ext root inside the image,
exactly as `update-dell.sh` does to the physical stick — but natively on the Pi (real
`mount`, no macOS `debugfs`/offset tricks).

Where rsync *does* help: the rare **full** push (`flash-dell-pi`) uses
`rsync --inplace --partial -z` instead of `scp` — resumable (Zero W + WiFi drop out) and
compressed on the wire (WiFi is the bottleneck; the image compresses well).

Where the real wall-clock goes, independent of transport:

| Stage | Cost | Note |
|---|---|---|
| Mac build | varies | fast path builds only `kernel.bin`+`i915.nkext` (from `_all`/`_kext`), **skips the ~320 MiB image regen** that `update-dell` currently forces via its `image64` dep |
| Transport (~6 MiB) | seconds | `scp -C` / `rsync -z`; not the cost |
| Pi surgery | seconds | write 6 MiB + reattach LUN |
| **Dell reboot/POST** | **tens of s** | **dominates the loop, unavoidable** |

## Architecture

### One-time Pi setup (`pi-bootstrap`)

1. Edit `/boot/firmware/config.txt` (with a `.bak`): `dr_mode=host` → `dr_mode=peripheral`.
   Ensure `dwc2` loads. Reboot once.
2. Install `/usr/local/bin/nanos-gadget.sh` + a systemd unit `nanos-gadget.service`
   that runs at boot and builds the configfs gadget:
   - `/sys/kernel/config/usb_gadget/nanos`: idVendor/idProduct (classic file-backed
     storage ids `0x0525`/`0xa4a5` so picky firmware treats it as a known stick),
     serial/manufacturer/product strings.
   - one `mass_storage.0` function, `lun.0.file = /home/pi/nanos/image64.img`,
     `removable = 1`.
   - bind to the UDC (`ls /sys/class/udc`).
3. Install `/usr/local/bin/nanos-apply.sh` (surgery helper) + a sudoers drop-in so `pi`
   can run the gadget/apply helpers without a password prompt over ssh.
4. Create `/home/pi/nanos/` (image + `staging/`).

### Pi surgery helper (`nanos-apply.sh`, runs as root)

Deterministic detach → modify → reattach, so the gadget re-opens the backing file and
serves the new bytes at the Dell's next boot:

```
set -euo pipefail
trap reattach EXIT                      # ALWAYS restore the LUN, even on failure
detach:    echo "" > <gadget>/functions/mass_storage.0/lun.0/file
loop:      LOOP=$(losetup -f --show -P /home/pi/nanos/image64.img)
           # wait for partition nodes (udevadm settle / partprobe)
mount:     mount the ext4 partition (detected via blkid TYPE=ext4, not hardcoded p3)
copy:      cp staging/kernel.bin  <root>/nanos/core/kernel.bin
           cp staging/i915.nkext  <root>/nanos/kext/i915.nkext
           printf 1 >             <root>/nanos/config/i915           # arm
verify:    stat each copied file; on-image size must equal staged size (short write = triple fault)
           config/i915 must read '1'
finish:    sync; umount; losetup -d $LOOP
reattach:  echo /home/pi/nanos/image64.img > <gadget>/.../lun.0/file
```

**Coherency reasoning:** clearing `lun.0/file` makes the mass-storage function close its
fd; re-pointing it reopens the file fresh, so the gadget serves the modified image. The
Dell re-reads the whole disk at each reboot (it must, to load the new kernel), so
mid-session host cache is a non-issue. Discipline: run updates only when the Dell is off
or rebooting — the same discipline as pulling a stick.

A `full` mode of the helper skips the mount/copy and instead accepts a freshly
rsync'd `image64.img` (detach → rsync-in-place already done by the Mac → reattach).

### Mac side (in-repo)

- `scripts/pi-bootstrap.sh` — idempotent one-time setup (copies helpers + unit, patches
  `config.txt` with backup, enables the service, prints "reboot the Pi").
- `scripts/pi-flash.sh` — full: build `image64` (dep), detach LUN, `rsync --inplace
  --partial -z image64.img pi:~/nanos/image64.img`, reattach. Mirrors `flash-dell-armed`.
- `scripts/pi-update.sh` — fast: build **only** kernel+kext (not the image), `scp -C`
  the two files to `pi:~/nanos/staging/`, `ssh … sudo nanos-apply.sh`. Mirrors
  `update-dell`.
- Makefile targets `pi-bootstrap`, `flash-dell-pi`, `update-dell-pi`; config
  `PI_HOST ?= pi@pendrak.local`, `PI_IMG ?= ~/nanos/image64.img`.
- `update-dell-pi` depends on the kernel+kext build only (a docker `make ARCH=x86_64
  _all _kext` style dep), **not** `image64` — dropping the redundant full-image regen
  from the hot loop.

## Error handling

- `nanos-apply.sh`: `set -euo pipefail`; `trap`-based LUN reattach on every exit path
  (never leave the Dell diskless); size-verify after each `cp`; arm-knob verify; bail if
  image/staging missing.
- Mac scripts: `ssh -o ConnectTimeout` reachability check; fail early on missing
  artifacts; propagate the Pi helper's exit code.
- `losetup -P` → `mount` race: `udevadm settle` / `partprobe` before mounting.
- Gadget service: log UDC-bind result to the journal; bind is to the UDC and does not
  depend on the Dell being present.

## Testing (no Dell required for most)

1. **Enumeration:** plug the Pi OTG port into the MacBook → `system_profiler
   SPUSBDataType` (or `lsusb`+`dmesg` on another Linux host) shows a removable USB disk
   of the correct size.
2. **Helper correctness:** run `nanos-apply.sh` against a throwaway copy of
   `image64.img`, then loop-mount and `cmp` the two target files against the staged
   artifacts.
3. **Built-in verify:** the size + arm-knob checks inside the helper.
4. **Integration (needs Dell):** boot the Dell from the gadget, `make i915-log`.

## Out of scope (YAGNI)

- Automatic Dell power/reset control — the user power-cycles the Dell manually.
- Dual-LUN, CDROM/ISO emulation.
- Delta-transfer of the full image — the build is not block-reproducible, so it would
  not pay off; the two-file fast path covers the common case.
- Ethernet-gadget / network-boot (iSCSI/NBD/iPXE) — the point is a plain USB stick.

# pendrak — Pi Zero W USB-gadget network boot device — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the MacBook push NanOS updates over WiFi to a Raspberry Pi Zero W that presents `image64.img` to the Dell as a USB mass-storage gadget, eliminating the physical Kingston-stick swap in the i915 inner loop.

**Architecture:** The Pi Zero W (`pi@pendrak.local`), independently powered and always on, runs a configfs mass-storage gadget whose backing file is `~/nanos/image64.img`; its OTG data port goes to a Dell USB port. The Mac keeps building as today and pushes either the whole image (`rsync --inplace`, rare) or just `kernel.bin`+`i915.nkext` (`scp`, then loop-mount surgery on the Pi — the fast loop). Pi-side scripts live in the repo under `scripts/pi/` and are installed by `scripts/pi-bootstrap.sh`.

**Tech Stack:** Raspbian trixie / kernel 6.18 (armv6l), dwc2 OTG + `libcomposite`/`usb_f_mass_storage` configfs gadget, systemd, `losetup -P`/`blkid`/`mount`, bash, rsync/scp over WiFi. Mac side: existing Makefile + Docker build (`make image64`).

## Global Constraints

- **Pi model/OS (verified 2026-07-05):** Raspberry Pi Zero W Rev 1.1, Raspbian trixie (Debian 13), kernel `6.18.34+rpt-rpi-v6`, armv6l. Single UDC — always bind via `$(ls /sys/class/udc)`, never hardcode a controller name.
- **Boot config file:** `/boot/firmware/config.txt` (trixie path, not `/boot/config.txt`).
- **Power/cable:** Pi has its OWN power (PWR micro-USB); only the OTG data port (micro-USB → USB-A) goes to the Dell. The gadget must be up before the Dell POSTs.
- **Backing image path on Pi:** `/home/pi/nanos/image64.img`. Staging dir: `/home/pi/nanos/staging/`.
- **Ext root paths inside the image (mirror `scripts/update-dell.sh`):** `/nanos/core/kernel.bin`, `/nanos/kext/i915.nkext`, arm knob `/nanos/config/i915` = `1`.
- **Mac artifact paths (mirror `update-dell` target, Makefile:1182-1183):** kernel `bin/k64/kernel.bin`, kext `bin/i915.nkext`; image `disk/image64.img` (`$(IMAGE64)`).
- **Gadget name:** `nanos` under `/sys/kernel/config/usb_gadget/`. LUN file node: `/sys/kernel/config/usb_gadget/nanos/functions/mass_storage.0/lun.0/file`.
- **No Claude/AI attribution in commit messages** (repo rule).
- **Update discipline:** only push updates while the Dell is off or rebooting (same discipline as pulling a stick); the Dell re-reads the whole disk at each boot, so host-cache coherency is a non-issue.
- **Idempotency:** every Pi-side script (gadget bring-up, bootstrap) must be safe to re-run.

## File Structure

Repo-versioned source of truth (installed onto the Pi by bootstrap):
- `scripts/pi/nanos-gadget.sh` — configfs gadget bring-up (→ `/usr/local/bin/` on Pi).
- `scripts/pi/nanos-gadget.service` — systemd unit that runs the gadget at boot.
- `scripts/pi/nanos-lun.sh` — tiny `{detach|attach}` LUN helper (→ `/usr/local/bin/`), whitelisted in sudoers so the full-flash path can toggle the LUN without a password.
- `scripts/pi/nanos-apply.sh` — surgical two-file writer (→ `/usr/local/bin/`).
- `scripts/pi/nanos-pi.sudoers` — NOPASSWD drop-in for `pi` (→ `/etc/sudoers.d/nanos`).

Mac-side drivers:
- `scripts/pi-flash.sh` — full image push (rsync).
- `scripts/pi-update.sh` — fast two-file push.
- `scripts/pi-bootstrap.sh` — one-time idempotent installer (patches `config.txt`, copies helpers, installs sudoers + service).
- `Makefile` — new targets `flash-dell-pi`, `update-dell-pi`, `pi-bootstrap`, `kernel-kext`, var `PI_HOST`.

---

### Task 1: Switch the Pi OTG port to device (peripheral) mode

**Files:**
- Modify (on Pi): `/boot/firmware/config.txt`

**Interfaces:**
- Produces: a bound UDC — `ls /sys/class/udc` is non-empty after reboot, enabling every later gadget task.

- [ ] **Step 1: Observe current state (expected: host mode, no UDC)**

Run:
```bash
ssh pi@pendrak.local 'grep -n dwc2 /boot/firmware/config.txt; echo "--- udc ---"; ls /sys/class/udc 2>&1'
```
Expected: shows `dtoverlay=dwc2,dr_mode=host`; `ls /sys/class/udc` prints nothing / "No such file" (host mode → no device controller).

- [ ] **Step 2: Patch config.txt to peripheral mode (with backup)**

Run:
```bash
ssh pi@pendrak.local 'sudo cp /boot/firmware/config.txt /boot/firmware/config.txt.nanos.bak && \
  sudo sed -i "s/dtoverlay=dwc2,dr_mode=host/dtoverlay=dwc2,dr_mode=peripheral/" /boot/firmware/config.txt && \
  grep -n dwc2 /boot/firmware/config.txt'
```
Expected: line now reads `dtoverlay=dwc2,dr_mode=peripheral`.

- [ ] **Step 3: [HUMAN] Reboot the Pi and wait for it to come back**

Run:
```bash
ssh pi@pendrak.local 'sudo reboot' || true
# wait for SSH to return
until ssh -o ConnectTimeout=5 pi@pendrak.local true 2>/dev/null; do sleep 3; done; echo "Pi back up"
```
Expected: "Pi back up" within ~40 s.

- [ ] **Step 4: Verify a UDC now exists**

Run:
```bash
ssh pi@pendrak.local 'ls /sys/class/udc'
```
Expected: prints a controller name, e.g. `20980000.usb` (non-empty = peripheral mode active).

- [ ] **Step 5: Commit (note the manual Pi change in the repo)**

No repo file changed yet; record the step so the history is legible:
```bash
git commit --allow-empty -m "chore(pendrak): Pi OTG port switched to dwc2 dr_mode=peripheral (config.txt on Pi, backed up to .nanos.bak)"
```

---

### Task 2: Bring up the mass-storage gadget manually and prove enumeration

**Files:**
- Create: `scripts/pi/nanos-gadget.sh`

**Interfaces:**
- Consumes: a bound UDC (Task 1).
- Produces: an enumerable USB mass-storage device backed by `/home/pi/nanos/image64.img`; the LUN file node path used by Tasks 4-6.

- [ ] **Step 1: Write the gadget bring-up script**

Create `scripts/pi/nanos-gadget.sh`:
```bash
#!/usr/bin/env bash
# nanos-gadget.sh — bring up (or refresh) the NanOS USB mass-storage gadget.
# Idempotent: tears down an existing "nanos" gadget before rebuilding.
set -euo pipefail

IMG="${1:-/home/pi/nanos/image64.img}"
G=/sys/kernel/config/usb_gadget/nanos

[ -f "$IMG" ] || { echo "backing image missing: $IMG" >&2; exit 1; }
modprobe libcomposite

# Idempotent teardown: unbind then remove any existing gadget tree.
if [ -d "$G" ]; then
  echo "" > "$G/UDC" 2>/dev/null || true
  rm -f "$G/configs/c.1/mass_storage.0" 2>/dev/null || true
  rmdir "$G/configs/c.1/strings/0x409" 2>/dev/null || true
  rmdir "$G/configs/c.1" 2>/dev/null || true
  rmdir "$G/functions/mass_storage.0" 2>/dev/null || true
  rmdir "$G/strings/0x409" 2>/dev/null || true
  rmdir "$G" 2>/dev/null || true
fi

mkdir -p "$G"
cd "$G"
echo 0x0525 > idVendor           # NetChip — classic file-backed storage vendor
echo 0xa4a5 > idProduct
echo 0x0100 > bcdDevice
echo 0x0200 > bcdUSB
mkdir -p strings/0x409
echo "NanOS"        > strings/0x409/manufacturer
echo "pendrak boot" > strings/0x409/product
echo "pendrak0001"  > strings/0x409/serialnumber
mkdir -p configs/c.1/strings/0x409
echo "mass_storage" > configs/c.1/strings/0x409/configuration
echo 250 > configs/c.1/MaxPower
mkdir -p functions/mass_storage.0
echo 1     > functions/mass_storage.0/lun.0/removable
echo 0     > functions/mass_storage.0/lun.0/ro
echo 0     > functions/mass_storage.0/lun.0/nofua
echo "$IMG" > functions/mass_storage.0/lun.0/file
ln -sf functions/mass_storage.0 configs/c.1/
ls /sys/class/udc > UDC
echo "gadget up: udc=$(cat UDC) file=$IMG"
```

- [ ] **Step 2: Stage a placeholder backing image and install the script on the Pi**

Run:
```bash
ssh pi@pendrak.local 'mkdir -p ~/nanos/staging && \
  { [ -f ~/nanos/image64.img ] || truncate -s 320M ~/nanos/image64.img; }'
scp scripts/pi/nanos-gadget.sh pi@pendrak.local:/tmp/nanos-gadget.sh
ssh pi@pendrak.local 'sudo install -m0755 /tmp/nanos-gadget.sh /usr/local/bin/nanos-gadget.sh'
```
Expected: no errors; a ~320 MiB placeholder exists so the gadget can bind before the real image is pushed (Task 4).

- [ ] **Step 3: Run the gadget script and verify it binds**

Run:
```bash
ssh pi@pendrak.local 'sudo /usr/local/bin/nanos-gadget.sh'
```
Expected: prints `gadget up: udc=20980000.usb file=/home/pi/nanos/image64.img`.

- [ ] **Step 4: [HUMAN] Plug the Pi OTG data port into the MacBook, verify enumeration**

Connect the Pi's **data** micro-USB (not PWR) to a MacBook USB port. Then on the Mac:
```bash
system_profiler SPUSBDataType | grep -A6 -i "pendrak\|NanOS\|Mass Storage"
```
Expected: a "pendrak boot" / "NanOS" mass-storage device (~320 MB). It may show as an unreadable/uninitialized disk in macOS — that is fine; we only need enumeration. Do NOT let macOS format it.

- [ ] **Step 5: Commit**

```bash
git add scripts/pi/nanos-gadget.sh
git commit -m "feat(pendrak): configfs mass-storage gadget bring-up script"
```

---

### Task 3: Auto-start the gadget at boot (systemd)

**Files:**
- Create: `scripts/pi/nanos-gadget.service`

**Interfaces:**
- Consumes: `/usr/local/bin/nanos-gadget.sh` (Task 2).
- Produces: the gadget comes up unattended after a Pi reboot (required so the Dell always finds a ready boot device).

- [ ] **Step 1: Write the systemd unit**

Create `scripts/pi/nanos-gadget.service`:
```ini
[Unit]
Description=NanOS USB mass-storage gadget (pendrak)
After=sys-kernel-config.mount
Requires=sys-kernel-config.mount

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/bin/nanos-gadget.sh /home/pi/nanos/image64.img
ExecStop=/bin/sh -c 'echo "" > /sys/kernel/config/usb_gadget/nanos/UDC 2>/dev/null || true'

[Install]
WantedBy=multi-user.target
```

- [ ] **Step 2: Install and enable the unit**

Run:
```bash
scp scripts/pi/nanos-gadget.service pi@pendrak.local:/tmp/nanos-gadget.service
ssh pi@pendrak.local 'sudo install -m0644 /tmp/nanos-gadget.service /etc/systemd/system/nanos-gadget.service && \
  sudo systemctl daemon-reload && sudo systemctl enable nanos-gadget.service'
```
Expected: `Created symlink … nanos-gadget.service`.

- [ ] **Step 3: [HUMAN] Reboot the Pi, then verify the gadget came up unattended**

Run:
```bash
ssh pi@pendrak.local 'sudo reboot' || true
until ssh -o ConnectTimeout=5 pi@pendrak.local true 2>/dev/null; do sleep 3; done
ssh pi@pendrak.local 'systemctl is-active nanos-gadget.service; cat /sys/kernel/config/usb_gadget/nanos/UDC'
```
Expected: `active`, and the UDC file prints the controller name (gadget bound with no manual step).

- [ ] **Step 4: Commit**

```bash
git add scripts/pi/nanos-gadget.service
git commit -m "feat(pendrak): systemd unit to bring the gadget up at boot"
```

---

### Task 4: Full image push from the Mac (`pi-flash.sh` + `flash-dell-pi`)

**Files:**
- Create: `scripts/pi/nanos-lun.sh`
- Create: `scripts/pi-flash.sh`
- Modify: `Makefile` (add `PI_HOST`, `flash-dell-pi`)

**Interfaces:**
- Consumes: the running gadget (Task 3), `$(IMAGE64)` = `disk/image64.img`.
- Produces: `nanos-lun.sh {detach|attach}` (used by full-flash and whitelisted in sudoers, Task 6); a `flash-dell-pi` target that rsyncs the real image onto the Pi.

- [ ] **Step 1: Write the LUN toggle helper**

Create `scripts/pi/nanos-lun.sh`:
```bash
#!/usr/bin/env bash
# nanos-lun.sh — detach/attach the mass-storage LUN backing file (fixed command for sudoers).
set -euo pipefail
LUN=/sys/kernel/config/usb_gadget/nanos/functions/mass_storage.0/lun.0
IMG=/home/pi/nanos/image64.img
case "${1:-}" in
  detach) echo "" > "$LUN/file" ;;
  attach) echo "$IMG" > "$LUN/file" ;;
  *) echo "usage: nanos-lun.sh {detach|attach}" >&2; exit 1 ;;
esac
```

Install it on the Pi:
```bash
scp scripts/pi/nanos-lun.sh pi@pendrak.local:/tmp/nanos-lun.sh
ssh pi@pendrak.local 'sudo install -m0755 /tmp/nanos-lun.sh /usr/local/bin/nanos-lun.sh'
```

- [ ] **Step 2: Write the Mac full-push script**

Create `scripts/pi-flash.sh`:
```bash
#!/usr/bin/env bash
# pi-flash.sh — push the whole image64.img to the pendrak Pi (rsync --inplace, resumable).
# Detaches the LUN so the mass-storage function releases the backing file, rsyncs in place,
# then reattaches. Run only while the Dell is off/rebooting.
set -euo pipefail
PI_HOST="${PI_HOST:-pi@pendrak.local}"
IMG="${IMG:-disk/image64.img}"
PI_IMG=/home/pi/nanos/image64.img

[ -f "$IMG" ] || { echo "missing $IMG — run 'make image64' first" >&2; exit 1; }
ssh -o ConnectTimeout=8 "$PI_HOST" 'mkdir -p ~/nanos' \
  || { echo "cannot reach $PI_HOST" >&2; exit 1; }

echo ">> detaching LUN"
ssh "$PI_HOST" 'sudo /usr/local/bin/nanos-lun.sh detach'
echo ">> rsync $IMG -> $PI_HOST:$PI_IMG"
rsync --inplace --partial -z --info=progress2 "$IMG" "$PI_HOST:$PI_IMG"
echo ">> reattaching LUN"
ssh "$PI_HOST" 'sudo /usr/local/bin/nanos-lun.sh attach'
echo "full image pushed to $PI_HOST. Boot the Dell."
```

- [ ] **Step 3: Add the Makefile target**

In `Makefile`, near the existing `flash-dell` target (~line 1132), add:
```make
PI_HOST ?= pi@pendrak.local

.PHONY: flash-dell-pi
flash-dell-pi: image64
	PI_HOST=$(PI_HOST) IMG=$(IMAGE64) ./scripts/pi-flash.sh
```

- [ ] **Step 4: Make scripts executable and run the full push**

Run:
```bash
chmod +x scripts/pi-flash.sh scripts/pi/nanos-lun.sh
make flash-dell-pi
```
Expected: rsync progress, then "full image pushed". First run transfers most of ~320 MiB; the point here is a correct end-to-end push.

- [ ] **Step 5: Verify the pushed image is intact on the Pi**

Run:
```bash
# host size vs pi size
echo "host: $(wc -c < disk/image64.img)"
ssh pi@pendrak.local 'echo "pi:   $(wc -c < ~/nanos/image64.img)"; \
  sudo losetup -f --show -P ~/nanos/image64.img > /tmp/loop.txt; L=$(cat /tmp/loop.txt); \
  for p in ${L}p*; do echo "$p $(sudo blkid -o value -s TYPE $p 2>/dev/null)"; done; \
  sudo losetup -d $L'
```
Expected: host and pi byte counts match; the loop shows an `ext4` partition (the NanOS root) and the ESP — proving the transferred image is the real partitioned NanOS disk, not the placeholder.

- [ ] **Step 6: Commit**

```bash
git add scripts/pi/nanos-lun.sh scripts/pi-flash.sh Makefile
git commit -m "feat(pendrak): full image push (flash-dell-pi) via rsync --inplace"
```

---

### Task 5: Fast two-file surgery on the Pi (`nanos-apply.sh`)

**Files:**
- Create: `scripts/pi/nanos-apply.sh`

**Interfaces:**
- Consumes: staged `/home/pi/nanos/staging/{kernel.bin,i915.nkext}`, the real image on the Pi (Task 4), the LUN node.
- Produces: `nanos-apply.sh` writes both files + arm knob into the ext root inside the image, verifies sizes, and always reattaches the LUN.

- [ ] **Step 1: Write the surgery helper**

Create `scripts/pi/nanos-apply.sh`:
```bash
#!/usr/bin/env bash
# nanos-apply.sh — surgically write staged kernel.bin + i915.nkext into the gadget backing
# image's ext root, re-arm i915, verify, and reattach the LUN. Root-only (loop/mount/configfs).
set -euo pipefail

IMG="${IMG:-/home/pi/nanos/image64.img}"
STAGE="${STAGE:-/home/pi/nanos/staging}"
ARM="${ARM:-1}"
LUN=/sys/kernel/config/usb_gadget/nanos/functions/mass_storage.0/lun.0
MNT=/mnt/nanos-root
LOOP=""

note(){ printf '\033[36m%s\033[0m\n' "$*"; }
die(){  printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }

reattach(){
  mountpoint -q "$MNT" && umount "$MNT" || true
  [ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null || true
  echo "$IMG" > "$LUN/file" 2>/dev/null || true   # never leave the Dell diskless
}
trap reattach EXIT

[ -f "$STAGE/kernel.bin" ] || die "missing $STAGE/kernel.bin"
[ -f "$STAGE/i915.nkext" ] || die "missing $STAGE/i915.nkext"
[ -f "$IMG" ]              || die "missing image $IMG"

# 1) detach the LUN so the mass-storage function closes its fd
echo "" > "$LUN/file"

# 2) loop-mount the ext4 root (auto partition nodes; detect ext4 rather than hardcode p3)
LOOP=$(losetup -f --show -P "$IMG")
udevadm settle 2>/dev/null || sleep 1
ROOTPART=""
for p in "${LOOP}"p*; do
  [ -b "$p" ] || continue
  [ "$(blkid -o value -s TYPE "$p" 2>/dev/null)" = ext4 ] && { ROOTPART="$p"; break; }
done
[ -n "$ROOTPART" ] || die "no ext4 partition found in $IMG"
mkdir -p "$MNT"
mount "$ROOTPART" "$MNT"

# 3) write the two files + the arm knob
install -D -m0644 "$STAGE/kernel.bin" "$MNT/nanos/core/kernel.bin"
install -D -m0644 "$STAGE/i915.nkext" "$MNT/nanos/kext/i915.nkext"
if [ "$ARM" = 1 ]; then mkdir -p "$MNT/nanos/config"; printf 1 > "$MNT/nanos/config/i915"; fi

# 4) verify sizes (a short write = a truncated kernel that triple-faults the Dell)
check(){ local s="$STAGE/$1" d="$MNT/nanos/$2" want got
  want=$(stat -c%s "$s"); got=$(stat -c%s "$d")
  [ "$want" = "$got" ] || die "size mismatch /nanos/$2: want $want got $got"
  note "  ok /nanos/$2 ($got B)"; }
check kernel.bin core/kernel.bin
check i915.nkext kext/i915.nkext
if [ "$ARM" = 1 ]; then
  [ "$(tr -d '\0' < "$MNT/nanos/config/i915")" = 1 ] || die "arm knob != 1"
  note "  ok /nanos/config/i915 = 1"
fi

sync
note "applied — kernel + i915 written; reattaching LUN. Boot the Dell, then 'make i915-log'."
# umount / losetup -d / reattach happen in the EXIT trap
```

- [ ] **Step 2: Install it on the Pi and stage test artifacts**

Run:
```bash
scp scripts/pi/nanos-apply.sh pi@pendrak.local:/tmp/nanos-apply.sh
ssh pi@pendrak.local 'sudo install -m0755 /tmp/nanos-apply.sh /usr/local/bin/nanos-apply.sh'
# stage the current host artifacts as the test payload
scp -C bin/k64/kernel.bin pi@pendrak.local:nanos/staging/kernel.bin
scp -C bin/i915.nkext      pi@pendrak.local:nanos/staging/i915.nkext
```
Expected: no errors. (If `bin/k64/kernel.bin`/`bin/i915.nkext` are absent, run `make image64` on the Mac first.)

- [ ] **Step 3: Run the surgery and verify size checks pass**

Run:
```bash
ssh pi@pendrak.local 'sudo ARM=1 /usr/local/bin/nanos-apply.sh'
```
Expected: `ok /nanos/core/kernel.bin (...)`, `ok /nanos/kext/i915.nkext (...)`, `ok /nanos/config/i915 = 1`, then "applied …". Exit code 0.

- [ ] **Step 4: Independently verify the bytes landed (cmp, no Dell)**

Run:
```bash
ssh pi@pendrak.local 'set -e; \
  L=$(sudo losetup -f --show -P ~/nanos/image64.img); \
  P=""; for p in ${L}p*; do [ "$(sudo blkid -o value -s TYPE $p)" = ext4 ] && P=$p; done; \
  sudo mkdir -p /mnt/verify && sudo mount $P /mnt/verify; \
  sudo cmp ~/nanos/staging/kernel.bin /mnt/verify/nanos/core/kernel.bin && echo "kernel OK"; \
  sudo cmp ~/nanos/staging/i915.nkext /mnt/verify/nanos/kext/i915.nkext && echo "i915 OK"; \
  sudo umount /mnt/verify; sudo losetup -d $L'
```
Expected: `kernel OK` and `i915 OK` (bytes on the image exactly match the staged files). Note the LUN was reattached by the apply trap before this check, so re-mounting read-only-ish here is only for verification while the Dell is absent.

- [ ] **Step 5: Verify the LUN was reattached (gadget still serves the image)**

Run:
```bash
ssh pi@pendrak.local 'cat /sys/kernel/config/usb_gadget/nanos/functions/mass_storage.0/lun.0/file'
```
Expected: `/home/pi/nanos/image64.img` (non-empty → the trap reattached correctly).

- [ ] **Step 6: Commit**

```bash
git add scripts/pi/nanos-apply.sh
git commit -m "feat(pendrak): loop-mount surgery helper (nanos-apply.sh) with LUN-reattach trap"
```

---

### Task 6: Fast push from the Mac + sudoers (`pi-update.sh`, `update-dell-pi`, `kernel-kext`)

**Files:**
- Create: `scripts/pi/nanos-pi.sudoers`
- Create: `scripts/pi-update.sh`
- Modify: `Makefile` (add `update-dell-pi`, `kernel-kext`)

**Interfaces:**
- Consumes: `nanos-apply.sh` (Task 5), Mac artifacts `bin/k64/kernel.bin`, `bin/i915.nkext`.
- Produces: `make update-dell-pi` — the unattended fast loop; a `kernel-kext` target that builds only kernel+kext (skips the ~320 MiB image regen).

- [ ] **Step 1: Write the sudoers drop-in and install it**

Create `scripts/pi/nanos-pi.sudoers`:
```
# NanOS pendrak — let 'pi' run the gadget helpers without a password (unattended fast loop).
pi ALL=(root) NOPASSWD: /usr/local/bin/nanos-apply.sh, /usr/local/bin/nanos-gadget.sh, /usr/local/bin/nanos-lun.sh
```

Install (validate with `visudo -c` before activating — a broken sudoers file locks you out):
```bash
scp scripts/pi/nanos-pi.sudoers pi@pendrak.local:/tmp/nanos-pi.sudoers
ssh pi@pendrak.local 'sudo install -m0440 /tmp/nanos-pi.sudoers /etc/sudoers.d/nanos && sudo visudo -c'
```
Expected: `/etc/sudoers.d/nanos: parsed OK` (and the rest OK).

- [ ] **Step 2: Verify NOPASSWD works over ssh (no TTY, no prompt)**

Run:
```bash
ssh -o BatchMode=yes pi@pendrak.local 'sudo -n /usr/local/bin/nanos-lun.sh attach && echo NOPASSWD-OK'
```
Expected: `NOPASSWD-OK` with no password prompt (`sudo -n` fails if a password would be required).

- [ ] **Step 3: Write the Mac fast-push script**

Create `scripts/pi-update.sh`:
```bash
#!/usr/bin/env bash
# pi-update.sh — fast i915 loop: push ONLY kernel.bin + i915.nkext to the pendrak Pi and run
# the surgical apply. Run only while the Dell is off/rebooting.
set -euo pipefail
PI_HOST="${PI_HOST:-pi@pendrak.local}"
KERNEL="${KERNEL:-bin/k64/kernel.bin}"
KEXT="${KEXT:-bin/i915.nkext}"
ARM="${ARM:-1}"

[ -f "$KERNEL" ] || { echo "missing $KERNEL — run 'make kernel-kext' first" >&2; exit 1; }
[ -f "$KEXT" ]   || { echo "missing $KEXT — run 'make kernel-kext' first" >&2; exit 1; }
ssh -o ConnectTimeout=8 "$PI_HOST" 'mkdir -p ~/nanos/staging' \
  || { echo "cannot reach $PI_HOST" >&2; exit 1; }

scp -C "$KERNEL" "$PI_HOST:nanos/staging/kernel.bin"
scp -C "$KEXT"   "$PI_HOST:nanos/staging/i915.nkext"
ssh "$PI_HOST" "sudo -n ARM=$ARM /usr/local/bin/nanos-apply.sh"
echo "update pushed to $PI_HOST — boot the Dell, then 'make i915-log'."
```

- [ ] **Step 4: Add Makefile targets (fast build + fast push)**

In `Makefile`, near `update-dell` (~line 1181), add:
```make
# kernel-kext — build ONLY the x86_64 kernel + kexts (no ~320 MiB image regen). Feeds update-dell-pi.
.PHONY: kernel-kext
kernel-kext:
	$(DOCKER_RUN) make ARCH=x86_64 _all _kext

# update-dell-pi — FAST loop over the network: push kernel + i915 kext to the pendrak Pi.
.PHONY: update-dell-pi
update-dell-pi: kernel-kext
	PI_HOST=$(PI_HOST) KERNEL=$(BINFOLDER)k64/kernel.bin KEXT=$(BINFOLDER)i915.nkext ARM=1 ./scripts/pi-update.sh
```

- [ ] **Step 5: Verify `kernel-kext` produces the expected artifacts**

Run:
```bash
make kernel-kext
ls -l bin/k64/kernel.bin bin/i915.nkext
```
Expected: both files exist with fresh timestamps. (If `_all _kext` names differ in this tree, adjust to the sub-targets the existing `update-dell` relies on — see Makefile:1182.)

- [ ] **Step 6: Run the full fast loop and verify**

Run:
```bash
chmod +x scripts/pi-update.sh
make update-dell-pi
```
Expected: two scp transfers, then the apply's `ok …` lines and "update pushed". Re-verify bytes as in Task 5 Step 4 if desired.

- [ ] **Step 7: Commit**

```bash
git add scripts/pi/nanos-pi.sudoers scripts/pi-update.sh Makefile
git commit -m "feat(pendrak): fast two-file push (update-dell-pi) + kernel-kext build + NOPASSWD sudoers"
```

---

### Task 7: One-time idempotent bootstrap (`pi-bootstrap.sh` + `pi-bootstrap`)

**Files:**
- Create: `scripts/pi-bootstrap.sh`
- Modify: `Makefile` (add `pi-bootstrap`)

**Interfaces:**
- Consumes: all `scripts/pi/*` assets.
- Produces: `make pi-bootstrap` reproduces the entire Pi-side setup (config.txt patch, helpers, sudoers, service) on a fresh Pi. Codifies the manual steps done in Tasks 1-6.

- [ ] **Step 1: Write the bootstrap script**

Create `scripts/pi-bootstrap.sh`:
```bash
#!/usr/bin/env bash
# pi-bootstrap.sh — one-time, idempotent setup of the pendrak Pi from a fresh Raspbian.
# Patches config.txt to device mode, installs the gadget/apply/lun helpers, sudoers, and the
# systemd unit. Safe to re-run. Requires one manual reboot (prints the reminder).
set -euo pipefail
PI_HOST="${PI_HOST:-pi@pendrak.local}"
SP="$(cd "$(dirname "$0")" && pwd)/pi"

ssh -o ConnectTimeout=8 "$PI_HOST" true || { echo "cannot reach $PI_HOST" >&2; exit 1; }

echo ">> config.txt: dwc2 -> peripheral (idempotent)"
ssh "$PI_HOST" 'set -e; f=/boot/firmware/config.txt; \
  [ -f "$f".nanos.bak ] || sudo cp "$f" "$f".nanos.bak; \
  sudo sed -i "s/dtoverlay=dwc2,dr_mode=host/dtoverlay=dwc2,dr_mode=peripheral/" "$f"; \
  grep -q "dtoverlay=dwc2,dr_mode=peripheral" "$f" || echo "dtoverlay=dwc2,dr_mode=peripheral" | sudo tee -a "$f" >/dev/null; \
  grep -n dwc2 "$f"'

echo ">> staging dir + placeholder image (if none yet)"
ssh "$PI_HOST" 'mkdir -p ~/nanos/staging && { [ -f ~/nanos/image64.img ] || truncate -s 320M ~/nanos/image64.img; }'

echo ">> install helpers"
for f in nanos-gadget.sh nanos-lun.sh nanos-apply.sh; do
  scp "$SP/$f" "$PI_HOST:/tmp/$f"
  ssh "$PI_HOST" "sudo install -m0755 /tmp/$f /usr/local/bin/$f"
done

echo ">> install sudoers (validated)"
scp "$SP/nanos-pi.sudoers" "$PI_HOST:/tmp/nanos-pi.sudoers"
ssh "$PI_HOST" 'sudo install -m0440 /tmp/nanos-pi.sudoers /etc/sudoers.d/nanos && sudo visudo -c'

echo ">> install + enable systemd unit"
scp "$SP/nanos-gadget.service" "$PI_HOST:/tmp/nanos-gadget.service"
ssh "$PI_HOST" 'sudo install -m0644 /tmp/nanos-gadget.service /etc/systemd/system/nanos-gadget.service && \
  sudo systemctl daemon-reload && sudo systemctl enable nanos-gadget.service'

echo "bootstrap done. If config.txt changed, REBOOT the Pi:  ssh $PI_HOST sudo reboot"
echo "Then push an image:  make flash-dell-pi"
```

- [ ] **Step 2: Add the Makefile target**

In `Makefile`:
```make
.PHONY: pi-bootstrap
pi-bootstrap:
	PI_HOST=$(PI_HOST) ./scripts/pi-bootstrap.sh
```

- [ ] **Step 3: Run bootstrap idempotently and confirm no drift**

Run (the Pi is already set up from Tasks 1-6; this proves re-running is safe):
```bash
chmod +x scripts/pi-bootstrap.sh
make pi-bootstrap
ssh pi@pendrak.local 'systemctl is-enabled nanos-gadget.service; sudo visudo -c | tail -1; grep dwc2 /boot/firmware/config.txt'
```
Expected: `enabled`, sudoers `parsed OK`, config shows `dr_mode=peripheral`, no errors — a second run changes nothing.

- [ ] **Step 4: Commit**

```bash
git add scripts/pi-bootstrap.sh Makefile
git commit -m "feat(pendrak): idempotent one-time Pi bootstrap (pi-bootstrap)"
```

---

### Task 8: Dell integration — boot from the gadget (human-in-the-loop)

**Files:** none (documentation + physical validation).

**Interfaces:**
- Consumes: a fully set-up Pi (Tasks 1-7) with a real image pushed (`make flash-dell-pi`).

- [ ] **Step 1: Prepare — Pi powered and gadget up before the Dell**

Ensure: Pi on its own power, gadget `active` (`ssh pi@pendrak.local systemctl is-active nanos-gadget.service` → `active`), OTG **data** port cabled to a Dell USB port. Push a fresh real image:
```bash
make flash-dell-pi
```

- [ ] **Step 2: [HUMAN] Boot the Dell from the gadget**

Power on the Dell; enter the boot menu (F12) and select the USB disk ("pendrak boot" / a USB mass-storage device). Confirm NanOS boots to init/shell.
Expected: NanOS boots off the Pi-backed USB disk. If the Dell does not list it, verify the Pi gadget was up *before* POST (power-cycle the Dell, not the Pi).

- [ ] **Step 3: [HUMAN] Run one full fast-loop iteration end to end**

With the Dell shut down (or at a reboot), from the Mac:
```bash
make update-dell-pi      # build kernel+kext, push, surgery on the Pi
```
Then power-cycle the Dell and:
```bash
make i915-log
```
Expected: the Dell boots the freshly-pushed kernel; `make i915-log` shows the new run — a complete cable-free iteration.

- [ ] **Step 4: Commit the workflow doc note**

Append a short "pendrak workflow" note to the spec or a README pointer, then:
```bash
git commit --allow-empty -m "docs(pendrak): Dell boot-from-gadget validated end-to-end"
```

---

## Self-Review

**Spec coverage:**
- Pi device-mode switch → Task 1. Gadget bring-up → Task 2. Auto-start → Task 3. Full rsync push → Task 4. Surgery helper + coherency trap → Task 5. Fast push + decoupled build + sudoers → Task 6. Idempotent bootstrap → Task 7. Dell integration → Task 8. Enumeration/`cmp`/size-verify tests → Tasks 2,4,5. Power-sequencing constraint → Global Constraints + Task 8. rsync-only-for-full / surgery-for-fast rationale → reflected in Tasks 4 vs 5-6.
- All spec sections map to a task. No gaps.

**Placeholder scan:** No TBD/TODO; every script is complete and runnable; every step has exact commands and expected output.

**Type/name consistency:** LUN node path, gadget name `nanos`, image path `/home/pi/nanos/image64.img`, staging `/home/pi/nanos/staging`, helper names (`nanos-gadget.sh`/`nanos-lun.sh`/`nanos-apply.sh`), and env vars (`PI_HOST`, `IMG`, `KERNEL`, `KEXT`, `ARM`) are identical across all tasks and match the spec's Global Constraints.

**Known adjustment point:** Task 6 Step 5 flags that `_all _kext` sub-target names may differ in this tree; the fallback is to mirror whatever the existing `update-dell` (Makefile:1182) depends on for `kernel.bin`/`i915.nkext`.

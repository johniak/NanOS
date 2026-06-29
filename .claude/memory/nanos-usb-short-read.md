---
name: nanos-usb-short-read
description: "Dell \"Unknown command nwlogin\" root cause — USB BOT data phase didn't verify full transfer"
metadata: 
  node_type: memory
  type: project
  originSessionId: 661d85b4-63ba-4deb-8adc-c850a1d3125e
---

The recurring Dell `toybox: Unknown command nwlogin` (tty7 graphical login never came up) was NOT the xHCI event-ring SMP race I first chased (those locks — g_xhciLock per-op + g_usbHcLock command-level — are valid hardening but were NOT the fix). **Real cause:** in `usb/UsbMsc.cpp` `bot()`, the CBW-out and CSW-in phases verified full length (`< 31`, `< 13`) but the **data phase only checked `< 0`**. `xhciSubmit` reports a SHORT bulk-IN (cc=13) as success with a partial byte count, so a short/failed 512 B sector read was accepted and the sector buffer kept its **stale prior contents**. Since init reads the greeter (`nwlogin.nxe`) right after a getty reads toybox (`login.nxe`), the greeter sector came back as toybox → `execve("/nanos/bin/nwlogin.nxe", {"nwlogin"})` ran toybox → "Unknown command nwlogin". QEMU always transfers the full 512 B (cc=1), so it never reproduced — masking it through every prior fix.

**Fix (branch feat/init-greeter-hardening):** (1) `bot()` data phase now requires `moved == dataLen`, else fails the command — **fails safe, never serves stale data**. (2) `drivers/UsbMscBlockDevice.cpp` retries each READ(10)/WRITE(10) up to 4× so a transient real-HW short read recovers within the read (WRITE(10) idempotent → safe). Verified: smoke-usb + smoke-usb-smp green (no-op in QEMU).

**Build-freshness gotcha (cost us cycles):** the on-screen init stamp (`NanOS init: build ...`) only bumps when `init.c` is recompiled — kernel-only fixes leave it frozen, so the user kept seeing the old "11:30" and couldn't tell if a build was new. To force a fresh stamp: `touch user/init.c` before `make image64`. The stamp is the **container UTC** time (e.g. 14:49 UTC = 16:49 CEST). This build: **Jun 26 2026 14:49 UTC**. Kernel-side markers also exist (xHCI "serialized" line, the new HWP line).

See [[nanos-usb-stack]], [[nanos-dual-boot-limine]], [[nanos-virtual-terminals]], [[nanos-cpu-power-hwp]].

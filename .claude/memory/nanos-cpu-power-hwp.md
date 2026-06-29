---
name: nanos-cpu-power-hwp
description: CPU power management — HWP/Speed-Shift enabled per-CPU to fix real-HW idle heat
metadata: 
  node_type: memory
  type: project
  originSessionId: 661d85b4-63ba-4deb-8adc-c850a1d3125e
---

Real Dell Latitude 5310 (Comet Lake) ran HOT at idle even though htop showed no load; QEMU never showed it. Root cause: NanOS did **zero** CPU power management — cores stayed pinned at the firmware hand-off frequency forever and only ever reached C1 via `hlt`. QEMU has no power model + doesn't expose HWP, so it looked fine there.

**Fix (branch feat/init-greeter-hardening):** `enableHwp()` in `arch/x86_64/cpu/cpu_x86_64.cpp`, called per-CPU from `cpuInit()` (BSP) and `archApCpuInit()` (APs) — the intel_pstate job. Gated on CPUID.06H:EAX[7] (HWP supported) so it's a **complete no-op in QEMU** (verified: smoke-x86_64 + smoke-smp still green). Sets IA32_PM_ENABLE(0x770)=1, then IA32_HWP_REQUEST(0x774): Min=lowest (from CAPABILITIES 0x771), Max=highest (no peak cap), Desired=0 (HW-autonomous), **EPP=0x80** (balanced). BSP prints one confirmation line at boot: `CPU: HWP/Speed-Shift enabled ... perf range L..H, EPP=0x80` (silent in QEMU). image64 rebuilt; awaiting Dell confirmation.

**Tuning knob if still warm on the Dell:** raise EPP toward power-saving (0x80 → 0xC0/0xFF).

**Why:** idle frequency/voltage scaling is the dominant idle-heat lever (P ~ f·V²); the 1000 Hz tick × N cores also blocks deep package C-states but tick==ms assumptions make lowering HZ risky, so HWP was the focused fix.

NOTE: idle path (`idleBody` → `arch::halt_or_hlt` = `sti; hlt`) and all APs DO halt correctly — no busy-spin. See [[nanos-real-hardware-roadmap]], [[nanos-dual-boot-limine]], [[nanos-smp-multicore]]. (User flagged a 2nd Dell bug, deferred — focus was the heat.)

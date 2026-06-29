---
name: nanos-meminfo-memtotal
description: "/proc/meminfo MemTotal fix — sum of usable RAM, not top address (killed phantom \"used\")"
metadata: 
  node_type: memory
  type: project
  originSessionId: 661d85b4-63ba-4deb-8adc-c850a1d3125e
---

Dell reported ~2.5 GiB "used" at idle (QEMU ~250 MiB) — both phantom. Root cause: `/proc/meminfo` MemTotal was `arch::bootMemTop()` = the **highest usable physical address**, not the sum of usable RAM. On real HW the span from low RAM to the top is full of PCI MMIO / ACPI/EFI-reserved / GPU-stolen holes, so `used = MemTotal − MemFree` counted those gigabytes of holes as "used." QEMU with small RAM has no remap/hole, so the effect was tiny there.

**Fix (branch feat/init-greeter-hardening, kernel/Kernel.cpp):** new `g_usableRamBytes`, summed in the boot free-marking pass (`markFreeAndCount` via `bootMemForEachUsable`, clamped to the bitmap span); `sysMemTotalKb()` now returns that. `bootMemTop()` is unchanged — still the top address, correct for sizing the frame bitmap + the huge-page identity map.

**Verified** with `smoke-bigmem` (6 GiB straddling the 3-4 GiB PCI hole): MemTotal 6,290,943 kB (≈ exact 6 GiB usable, was ~7 GiB top), MemFree 5.70 GiB → **used ≈ 308 MiB** (was ~1.3 GiB on this box / ~2.5 GiB on Dell). smoke-x86_64 + smoke-bigmem green.

**Residual ~308 MiB used = real reservations, NOT phantom:** kernel heap (`heapSize = topOfRam/4`, capped [8 MiB, 256 MiB], `markRangeUsed` in full at `arch/x86_64/mm/mmu_x86_64.cpp:76`) + 32 MiB exec-staging window + kernel image + low 1 MiB. On the 6 GiB box the heap is 256 MiB but **98% free** (KHeapFree 253/256). If a smaller footprint is wanted, shrink the heap cap — but heavy ports (openssl/git/vim/doom) lean on it, so don't shrink blindly. See [[nanos-cpu-power-hwp]], [[nanos-x64-paging-carryforward]].

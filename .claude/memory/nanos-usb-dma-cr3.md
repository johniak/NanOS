---
name: nanos-usb-dma-cr3
description: Bug
metadata: 
  node_type: memory
  type: project
  originSessionId: 661d85b4-63ba-4deb-8adc-c850a1d3125e
---

The SECOND Dell bug (the one the user flagged as "there are two bugs"), exposed once bug #1 (the per-CPU execve fix, [[nanos-exec-staging-race]]) let the greeter/nwm actually run. Symptom on real HW: `KERNEL EXCEPTION vec=0e err=0x2 rip=0x144e5e cs=0x8 cr2=0x28133c0` — a #PF, **write to a not-present page in kernel mode**, in `arch::ringPush` (xhci_x86_64.cpp).

**Cause:** the xHCI rings/contexts/event-ring are kernel frames the CPU touches via `phys==virt`. That identity only holds under the kernel CR3. A per-process address space PRIVATIZES the user-window VA range **[8 MiB, 64 MiB)** (`mmu_x86_64.cpp` `dropUserWindows`; the still-identity ranges in a user space are [0,8 MiB) and **[64 MiB, 1 GiB)** — the kernel heap lives in the latter, which is why heap access under a user CR3 works). A USB transfer issued from a USER process (a file read off the USB root by the greeter/nwm) runs on that process's CR3, so a ring frame that landed in [8,64) MiB was remapped to the user window → fault. QEMU's rings landed in [64 MiB,1 GiB) so it never faulted naturally; only real HW (Dell) put a ring at ~42 MiB.

**Fix (branch feat/init-greeter-hardening, commit 3d5ab59):** `KernelCr3` RAII guard in `xhciSubmit`/`xhciIntPoll`/`xhciConfigureEndpoint` — switch to the kernel directory for the access, restore the caller's CR3 after (no-op when already on kernel CR3, e.g. enumeration + the HID poll thread). Same pattern `mmuMapUserFb` already used. enablePort runs at enumeration (kernel CR3) so needs no guard.

**Proven (the user demanded certainty + tests):** `XHCI_TEST_FORCE_WINDOW` (off by default) forces an endpoint ring into [8,64) MiB. A/B in QEMU: guard OFF → reproduced the exact fault (vec=0e, cr2 in window); guard ON → clean boot, 0 exceptions. Permanent regression gate `scripts/smoke-usb-dmawindow.sh` (+ `KEXTRA` make hook) builds with the forcing and asserts root-on-USB + smp2 reaches login with no exception; wired into `verify64`.

Shipping build stamp **Jun 27 2026 13:15 UTC**. Bug #1 (toybox-as-nwlogin) confirmed FIXED on the Dell in the prior boot. See [[nanos-usb-stack]], [[nanos-x64-paging-carryforward]], [[nanos-cpu-power-hwp]], [[nanos-meminfo-memtotal]].

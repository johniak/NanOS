---
name: nanos-real-hardware-roadmap
description: "18-24mo roadmap for NanOS as a real-hardware daily-driver (target now Dell Latitude 5310); main doc docs/superpowers/ROADMAP.md; team of 30"
metadata: 
  node_type: memory
  type: project
  originSessionId: a199f31d-5457-4061-8fb5-78971ea5cb57
---

NanOS strategic roadmap. **Authoritative current doc: `docs/superpowers/ROADMAP.md`** (2026-06-16,
labeled "główny roadmap NanOS"). The older `docs/superpowers/specs/2026-06-15-real-hardware-roadmap-design.md`
is the SUPERSEDED first design (different target machine — see below).

**North star (target machine CHANGED):** the current ROADMAP.md targets a **Dell Latitude 5310**
(Comet Lake, 2020; i5-10310U 4C/8T or i3-10110U 2C/4T — unit TBC): **NVMe (M.2) is the PRIMARY disk**,
native RJ45 (e1000e/I219-LM-or-V), xHCI USB, i8042 keyboard, possibly **UEFI Class 3 (no CSM)** — so
the cheap CSM+Multiboot1 bootstrap may be unavailable and UEFI is needed earlier (confirm in BIOS).
The earlier design spec said **HP EliteBook 820 G3** (Skylake 2C/4T, SATA primary, UEFI+CSM) — STALE.
Horizon ~18-24 months, team of **30 people**, 7→8 parallel streams, 8-quarter backbone.

**Boot + USB (per ROADMAP.md):** boot path = **UEFI → ESP (GPT/FAT)** from the internal disk (NVMe),
Stream D — there is NO dedicated "boot from a USB stick" item (same UEFI/ESP mechanism would apply).
USB is Stream F (Q4): **xHCI + USB core + HID + mass storage (bulk-only, SCSI read)** — for using/
mounting USB drives at runtime, as the flashing medium ("storage do flashowania"), and as the fallback
package-install source (local ZIP on disk/USB, Stream J Q4). So "run/use from USB" = planned (Q4);
"boot the laptop off a USB stick" = not an explicit roadmap line.

**Sequencing (approach B):** x86_64 migration is the gated-first foundation (small elite
team on the ~1500-2000-line critical path); everyone else builds MI/host-testable
components (USB core, COW, SMP lock audit, finish pthread, toolchain) in parallel from
day 1. Builds on [[nanos-x64-migration]] analysis and [[nanos-multiprocessing-roadmap]].

**Repo governance (decided):** the pristine **zero-AI original** is `master`@`b83b433`
(2026-02-05) — preserve via tag `original-zero-ai-2026-02-05` + protected branch
`legacy/original-zero-ai` BEFORE moving `master`. Then `master`→ new `main` (real-HW
"high" trunk) + `develop` (integration); per-agent `feat/*` each in its own git worktree;
a long-lived worktree pinned to `main` (`../nanos-hw-release`) feeds the hardware lab.
Per-worktree build-infra needed (disk image / qmon socket / Docker container suffixed) so
30 parallel QEMU runs don't collide. See [[no-claude-attribution-in-commits]].

**LinuxKPI (user requirement):** FreeBSD-style kernel source-compat shim to reuse Linux
drivers (GPU i915, WiFi iwlwifi) — Stream H. Device model must be "Linux-shaped" from
Q3-Q4 (SMP locks, PCIe/DMA/IRQ semantics) so the shim stays thin. GPL → keep Linux
drivers as separate modules (drm-kmod pattern). PoC in Q8, full drivers post-1.0.

# Real-hardware GPU bring-up (Dell Latitude 5310, i915)

NanOS drives the **unmodified Linux 6.12 i915 DRM driver** on the Dell Latitude 5310 (Comet Lake-U,
UHD Gen9.5, PCI `8086:9B41`) through the LinuxKPI shim. This is the recurring workflow and safety
protocol for bring-up sessions. See the design in `docs/superpowers/plans/2026-07-01-plan-3-gl-on-dell-i915-iris.md`
and the running results in `docs/superpowers/plans/2026-07-01-dell-gpu-test-log.md`.

## The arm knob (opt-in, safe by default)

`i915.nkext` ships in the default x86_64 image but is a **safe no-op** unless armed: the first byte of
`/nanos/config/i915` (on the stick's ext root; the kernel sees it at `/disks/main/nanos/config/i915`)
must be `1`. Unarmed, `nkext_init` returns before touching mem_map / DRM core / `i915_init`, so it can
never fault the box or double-init DRM core against another driver. This means the same image boots
normally on any machine and only attempts the GPU when you explicitly arm it.

## No serial port → two log channels

The Dell has no serial port, so every marker is captured two ways, always in lockstep:

1. **fbcon** (`knx_log`) — visible on screen; photograph it if the machine hangs.
2. **persistent file** (`/nanos/logs/i915-boot.txt`) — survives a hang + reboot. When armed, the
   harness also tees **every** `printk` line (the full `drm_dbg` trail, with `__drm_debug=0x1ff`) into
   this file, so even a probe that scrolls the screen or hard-hangs leaves the complete narration.

## The macOS workflow (auto-finds the Kingston stick)

All targets locate the boot stick by media name (`Kingston|DataTraveler`), refuse fixed/multiple
disks, and use host `debugfs` on the ext partition (needs `sudo`; `brew install e2fsprogs`).

| Command | Effect |
|---|---|
| `make flash-dell-armed` | build `image64` → `dd` to the stick → arm — one shot (flash before arm; a bare `make flash-dell` leaves it unarmed) |
| `make arm-i915` / `make disarm-i915` | flip `/nanos/config/i915` in place, no reflash |
| `make i915-inject N=<stage>` / `make i915-inject-off` | dial-a-stop (see below), no reflash |
| `make i915-log` | dump `/nanos/logs/i915-boot.txt` after a boot |

Boot loop: `make flash-dell-armed` → move stick to the Dell → boot → (photograph the screen if it
hangs) → move stick back → `make i915-log` → read the trail → adjust → repeat.

## Dial-a-stop (`inject_probe_failure`) — clean early exit without editing i915

`i915.modeset=0` is the *wrong* knob: it makes `i915_init()` itself return `-ENODEV`, so the driver
never registers. To stop the **probe** early instead, the glue uses i915's own
`inject_probe_failure` — a driver-native mechanism that aborts probe through the driver's own
`-ENODEV` unwind at its Nth internal injection point (designed for testing error paths). The stop
stage is read at runtime from `/nanos/config/i915_inject`:

- `make i915-inject-off` (or the file absent / `0`) → **full probe** (default): go as far as the
  hardware allows in one boot — maximum signal.
- `make i915-inject N=3` → abort cleanly at injection point 3. After a full-probe crash, dial `N`
  down to the last clean stage to confirm the unwind path, with no rebuild/reflash.

## Safety protocol

- The firmware framebuffer is the fallback throughout. Nothing hands the display to i915 until the
  KMS milestone (plan Task 7); until then a probe failure or early stop leaves the firmware fb intact.
- Every marker failure returns `0` from `nkext_init` — the harness never faults the box.
- Record every Dell session in the test log: date, image commit, transcribed/photographed values,
  and the fallback state. No "seemed to work" entries.

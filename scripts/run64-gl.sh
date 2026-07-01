#!/usr/bin/env bash
# run64-gl.sh — boot image64 with virgl 3D (virtio-vga-gl + virglrenderer/ANGLE/Metal on macOS).
#
# The GL-capable QEMU comes from the startergo/qemu-virgl-kosmickrisp tap. That tap ships its
# build AS the `qemu` formula, so after `brew install startergo/qemu-virgl-kosmickrisp/qemu`
# the plain `qemu-system-x86_64` on PATH *is* the virgl QEMU (it is a strict superset — it still
# runs the plain 2D `-device virtio-gpu` path used by scripts/smoke-virtio-gpu.sh).
#
#   Taps needed (the qemu formula pulls these in):
#     brew tap startergo/qemu-virgl-kosmickrisp startergo/virglrenderer \
#              startergo/angle startergo/libepoxy startergo/gn
#     HOMEBREW_NO_REQUIRE_TAP_TRUST=1 brew install startergo/qemu-virgl-kosmickrisp/qemu
#
# Env:
#   QEMU_GL          path to a virgl-capable qemu-system-x86_64 (default: first on PATH).
#   DISPLAY_BACKEND  cocoa,gl=es (default, interactive) | none (headless smoke).
#   SER              serial log path (default /tmp/nanos-gl.log).
#
# NOTE (Apple Silicon): brew's bottle relocation invalidates the code signature of the tap's
# libepoxy/angle/virglrenderer dylibs, which makes the kernel SIGKILL qemu at launch
# ("Code Signature Invalid / Invalid Page"). We ad-hoc re-sign them below, idempotently, so a
# fresh `brew reinstall` self-heals on the next run.
set -u
IMG=disk/image64-grub2.img
SER=${SER:-/tmp/nanos-gl.log}
QEMU_GL=${QEMU_GL:-$(command -v qemu-system-x86_64)}
[ -n "$QEMU_GL" ] && [ -x "$QEMU_GL" ] || { echo "no virgl qemu (set QEMU_GL)"; exit 2; }
[ -f "$IMG" ] || { echo "run 'make image64' first"; exit 2; }

# Self-heal: re-sign any tap dylib whose signature the bottle relocation invalidated.
if command -v codesign >/dev/null 2>&1 && command -v brew >/dev/null 2>&1; then
    for keg in libepoxy angle virglrenderer; do
        d=$(brew --prefix "startergo/$keg/$keg" 2>/dev/null)/lib
        [ -d "$d" ] || continue
        for lib in "$d"/*.dylib; do
            [ -f "$lib" ] || continue
            codesign -v "$lib" >/dev/null 2>&1 || codesign --force --sign - "$lib" >/dev/null 2>&1
        done
    done
fi

# virtio-vga-gl exposes both the 2D scanout and the virgl 3D contexts. On macOS the host GL
# backend is ANGLE's ES profile (gl=es); if that errors, try gl=core.
exec "$QEMU_GL" -accel tcg,thread=multi -cpu qemu64 -smp 4 -m 512 \
    -drive file="$IMG",format=raw \
    -device virtio-vga-gl \
    -display "${DISPLAY_BACKEND:-cocoa,gl=es}" \
    -serial file:"$SER" \
    -monitor unix:/tmp/nanos-gl-qmon.sock,server,nowait -no-reboot "$@"

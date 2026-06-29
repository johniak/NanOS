#!/usr/bin/env bash
# run64-gl.sh — boot the x86_64 NanOS image with virtio-gpu 3D (virgl) acceleration.
#
# macOS has no installable virglrenderer and QEMU's macOS GL backends do not drive virgl
# headlessly, so the GL-capable QEMU lives in the `nanos-gltest` Linux container (Debian's
# qemu-system-x86_64 ships `virtio-vga-gl`; Mesa llvmpipe + Xvfb give a software GL context
# with no GPU). The guest virgl stream is identical to a real-GPU host, so this verifies the
# whole 3D path; move to a native Linux box with a real GPU for performance.
#
# Usage:
#   bash scripts/run64-gl.sh            # boot + wait for login, then tail serial
#   MODE=shot bash scripts/run64-gl.sh  # boot, log in on tty7, screendump disk/nanos-gl.ppm
# Extra args after -- go to qemu.
set -u
cd "$(dirname "$0")/.."
IMG=disk/image64-grub2.img
[ -f "$IMG" ] || { echo "run 'make image64' first ($IMG missing)"; exit 2; }

docker image inspect nanos-gltest >/dev/null 2>&1 || \
    docker build --platform linux/arm64 -t nanos-gltest -f docker/Dockerfile.gltest . || exit 1

exec docker run --rm -i -v "$PWD":/work -w /work \
    -e MODE="${MODE:-wait}" -e BOOT_TIMEOUT="${BOOT_TIMEOUT:-180}" \
    nanos-gltest bash scripts/_gl-run-inner.sh "$@"

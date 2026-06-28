#!/usr/bin/env bash
# vendor-linux.sh — fetch the Linux 6.12 tarball and extract ONLY the files the LinuxKPI
# build compiles (virtio core, later DRM core + virtio_gpu) into external/linux-6.12/.
#
# Vendored Linux source is GPLv2 and is kept in its own directory, compiled but never
# modified and never linked into kernel.bin (only into virtio_gpu.nkext). The license
# boundary is the LinuxKPI interface (see docs/superpowers/specs/...-linuxkpi-virtio-gpu).
#
# The tarball is cached (LINUX_TARBALL_CACHE) so re-runs don't re-download. Idempotent:
# safe to re-run as later phases extend FILES.
set -eu

VER=6.12
SHA256=b1a2562be56e42afb3f8489d4c2a7ac472ac23098f1ef1c1e40da601f54625eb
URL="https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-${VER}.tar.xz"
CACHE="${LINUX_TARBALL_CACHE:-${TMPDIR:-/tmp}/linux-${VER}.tar.xz}"
DEST="external/linux-${VER}"

# Files to extract (paths relative to the tarball's linux-6.12/ root). Grows per phase.
FILES=(
  # --- P1: virtio core ---
  drivers/virtio/virtio.c
  drivers/virtio/virtio_ring.c
  drivers/virtio/virtio_pci_common.c
  drivers/virtio/virtio_pci_common.h
  drivers/virtio/virtio_pci_modern.c
  drivers/virtio/virtio_pci_modern_dev.c
  include/linux/virtio.h
  include/linux/virtio_ring.h
  include/linux/virtio_config.h
  include/linux/virtio_byteorder.h
  include/linux/virtio_anchor.h
  include/linux/virtio_pci_modern.h
  include/linux/virtio_pci_legacy.h
  include/uapi/linux/virtio_ring.h
  include/uapi/linux/virtio_config.h
  include/uapi/linux/virtio_pci.h
  include/uapi/linux/virtio_ids.h
  include/uapi/linux/virtio_types.h
  include/uapi/linux/virtio_gpu.h
  # COPYING for the GPL boundary record
  COPYING
)

echo "vendor-linux: target Linux ${VER}"

if [ ! -f "$CACHE" ]; then
  echo "vendor-linux: downloading $URL"
  curl -L --fail -o "$CACHE.tmp" "$URL"
  mv "$CACHE.tmp" "$CACHE"
fi

echo "vendor-linux: verifying sha256"
got=$(shasum -a 256 "$CACHE" | awk '{print $1}')
if [ "$got" != "$SHA256" ]; then
  echo "vendor-linux: SHA256 MISMATCH: got $got want $SHA256" >&2
  exit 1
fi

mkdir -p "$DEST"
echo "vendor-linux: extracting ${#FILES[@]} paths into $DEST"
for p in "${FILES[@]}"; do
  if tar -xJf "$CACHE" -C "$DEST" --strip-components=1 "linux-${VER}/$p" 2>/dev/null; then
    :
  else
    echo "  (skip, not found: $p)"
  fi
done

cat > "$DEST/VERSION-PROVENANCE" <<EOF
Linux kernel source, vendored for the NanOS LinuxKPI build.

  version: ${VER}
  source : ${URL}
  sha256 : ${SHA256}

These files are GPLv2 (see COPYING). They are compiled UNMODIFIED against the LinuxKPI
shim (linuxkpi/) and linked only into virtio_gpu.nkext — never into kernel.bin. The
license boundary is the LinuxKPI interface. Regenerate with scripts/vendor-linux.sh.
EOF

echo "vendor-linux: done. Extracted tree under $DEST:"
find "$DEST" -type f | sort | sed 's/^/  /'

# Third-Party Licenses

NanOS bundles or statically links the following third-party open-source software.
Their license texts are kept verbatim under `licenses/`. The kernel itself is the
project's own code; this file covers external components used by the **userland**.

## picolibc

- **What:** C library for the NanOS userland (`stdio`/`printf`/`malloc`/`qsort`/
  `string`/`time`). Statically linked into the `.nxe` programs (`cat`, `ls`, `nsh`),
  which are written into the bootable disk image.
- **Version:** 1.8.6 (pinned in `docker/Dockerfile`, `ARG PICOLIBC_VERSION`).
- **Upstream:** https://github.com/picolibc/picolibc
- **License:** BSD-style. picolibc is a collection of BSD-2/BSD-3-Clause and related
  permissive licenses (it descends from Newlib). The complete per-file copyright and
  license listing is reproduced verbatim in [`licenses/picolibc-COPYING.txt`](licenses/picolibc-COPYING.txt).
- **How it's obtained:** cloned and built from source in the `nanos-build` Docker
  image (not vendored into this repo); the resulting `libc.a` is linked at userland
  build time.

<!-- sbase (cat/ls + libutil + libutf) is added when those programs are vendored. -->

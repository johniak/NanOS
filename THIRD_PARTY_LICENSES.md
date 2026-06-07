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

## sbase (suckless base utilities)

- **What:** the `cat` and `ls` coreutils, vendored **verbatim** from upstream, plus
  the `libutil`/`libutf` support files and the `arg.h`/`util.h`/`compat.h` headers
  they need. Compiled against picolibc into `/bin/cat.nxe` and `/bin/ls.nxe`.
- **Vendored at:** `user/third_party/sbase/` (upstream commit recorded in
  `user/third_party/sbase/VERSION`).
- **Upstream:** https://git.suckless.org/sbase
- **License:** ISC/MIT-style. The upstream `LICENSE` file is kept verbatim at
  [`user/third_party/sbase/LICENSE`](user/third_party/sbase/LICENSE).
- **Local changes:** none to the program sources — `cat.c`/`ls.c` and the `libutil`/
  `libutf` files are unmodified upstream. Only the build (Makefile) and the libc
  porting layer (`user/libc-glue/`) are NanOS's own.

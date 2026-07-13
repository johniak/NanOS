# ports/node — Node.js for NanOS x86_64

Node.js (and its embedded V8) cross-compiled for NanOS, the JavaScript runtime Electron embeds
(plan `docs/superpowers/plans/2026-07-07-electron-marktext/02-node-v8.md`). One V8/Node port serves
both a standalone `node.nxe` and Electron.

## Layout

- `nx-node-cc`, `nx-node-cxx` — cross-compiler wrappers. Node's gyp build (`--dest-os=linux`) hands
  the target compiler glibc/Linux flags the picolibc-based `x86_64-nanos` gcc rejects; the wrappers
  strip exactly those (each stripped flag documented in `docs/en/node-port.md`) and forward the rest.
- `patches/` — source patches applied by `scripts/electron/build-node.sh` before configure, each with
  the standard port-patch header (see EXECUTION_GUIDE). NanOS-specific code is `#ifdef __NANOS__`.
- `addon-smoke/` — the smallest statically-linked native addon, proving the no-`dlopen` addon path
  (plan 02 Task 2.5).

## Build

Source lives at `$SDK_WORK/node-src` (fetched by `scripts/electron/fetch-node.sh`). From the repo:

```sh
make node        # refreshes the x86_64-nanos sysroot, cross-builds in the nanos-sdk-dev container,
                 # converts the ELF to bin/node.nxe, stages it for the image
```

`scripts/electron/build-node.sh` is the record of the exact configure + make invocation. Native
addons are statically registered (`NODE_MODULE_LINKED` + `--link-module`); there is no `.node`
`dlopen` (NanOS has no ELF `.so` loader — a `.ndl`-backed `process.dlopen` is a documented follow-up).

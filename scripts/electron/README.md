# scripts/electron — Electron stack build entry points

These scripts are the host-side build/packaging entry points for the NanOS Electron program
(Node → Chromium → Electron runtime → generic packager). Big upstream sources (Chromium, Electron,
Node, MarkText) live in `$SDK_WORK` (`~/Projects/nanos-sdk-work` by default) and are **never**
vendored into this repo — only recipes, patches, manifests, and build scripts are versioned.
Patched upstreams live as forks under `github.com/NanOS-labs` (branch `nanos`); this directory
holds the generic, app-agnostic packager glue that is NanOS platform code, not an upstream fork.

Contract, layout, and pinned versions: [`docs/en/electron-platform.md`](../../docs/en/electron-platform.md)
and [`manifest/electron-stack.lock`](../../manifest/electron-stack.lock).

Scripts are added by their owning plan (02–07); none exist yet at M0.

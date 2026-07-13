# Electron Apps on NanOS - MarkText Acceptance Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans. This is a multi-plan program, not one patch.
> Execute the child plans in dependency order and update this file after every completed gate.

**Goal:** NanOS can build and run Electron applications as first-class `.nxe` app bundles. The
acceptance application is **MarkText**: the MarkText source tree builds through the NanOS toolchain,
packages into a NanOS app bundle, and launches as a working GUI editor on NanOS.

**Broader success criterion:** MarkText is the proof, not a one-off. The resulting platform must
also support other Electron apps that use the normal Electron model:

- app JavaScript/HTML/CSS bundled on the host or in the SDK;
- an Electron runtime `.nxe` shared across apps;
- one generic app bundle format under `/disks/main/apps/<app>/`;
- a reusable `electron-nanos-packager` flow that turns an Electron app directory into a NanOS
  bundle without editing the Electron runtime.

**Current upstream baseline to pin first:** MarkText `master` currently reports version `0.19.0`,
`packageManager: pnpm@10.33.4`, Node `>=20.19.0`, and Electron `^42.1.0` in `package.json`. Pin an
exact MarkText commit and exact resolved Electron version in Task 0 before any build work; do not
track floating npm ranges.

**Architecture:** Treat Electron as a platform stack:

```
NanOS kernel/libc/syscalls/devices
  -> POSIX/runtime gap closure for browser-class programs
  -> Node.js + V8 target support
  -> Chromium content shell on NanOS display/input/sandbox surface
  -> Electron runtime (`electron.nxe`)
  -> generic app packaging (`electron-nanos-packager`)
  -> MarkText bundle (`/apps/marktext/MarkText.nxe` or launcher + app.asar)
```

This plan deliberately creates a reusable Electron substrate. MarkText-specific patches are allowed
only in the MarkText child plan; every runtime/platform change belongs in Node, Chromium, Electron,
or NanOS platform-gap plans.

## Global Constraints

- **No shortcuts that make only MarkText work.** Any app-specific workaround must live in
  `ports/marktext/` with a comment explaining why it is not platform code.
- **Pin everything.** Chromium/Electron/Node/MarkText/npm artifacts get commit or version + SHA256
  in a manifest. No `latest`, no floating `^` at build time.
- **Do not vendor huge browser sources into this NanOS repo.** Recipes, patches, manifests, and
  build scripts are versioned; Chromium/Electron/Node source checkouts live in reproducible sibling
  work dirs populated by bootstrap scripts.
- **x86_64 only.** This program targets `make image64` and the existing x86_64 user ABI.
- **Use the existing NanOS stack first.** Display/input should target NanWM/DRM through existing
  devices and libraries where possible; add kernel APIs only when Chromium/Electron actually needs
  a Linux-compatible primitive.
- **Every change or addition ships with written tests — no exceptions.** Any new kernel API,
  syscall, libc-glue function, script, or build target lands in the same commit as its tests:
  host unit tests in `tests/` where the logic is host-testable, a `user/<name>.c` microtest plus
  QEMU smoke for kernel/runtime behavior, and a smoke script for every new gate command. A change
  without a test that fails before it and passes after it is not done.
- **Headless gates before GUI gates.** Node tests and Chromium content smoke must pass before
  trying Electron, and Electron hello-world must pass before MarkText.
- **Every child plan has a runnable smoke.** Junior implementers should always know the next command
  and expected output.
- **Decisions are closed.** Child plans contain `Decisions` sections with choices already made.
  Executors do not re-open them; if one proves impossible, follow the Blocker Rules in
  [EXECUTION_GUIDE.md](EXECUTION_GUIDE.md) and update the plan file in the same commit.
- **Names are a contract.** Make targets, script names, and install paths come from the Command
  Name Contract and Path Contract in [EXECUTION_GUIDE.md](EXECUTION_GUIDE.md).
- **MarkText is the acceptance app.** The final PR is not complete until MarkText launches on NanOS,
  opens a markdown file, edits text, saves it, and survives app restart.

## Created Structure

This directory contains the execution plans:

| Plan | Purpose |
|---|---|
| [EXECUTION_GUIDE.md](EXECUTION_GUIDE.md) | How a junior should execute the program: workflow, handoff artifacts, gate commands, blocker rules. |
| [00-platform-contract.md](00-platform-contract.md) | Define bundle format, manifests, source pins, app ABI, and final gates. |
| [01-nanos-platform-gaps.md](01-nanos-platform-gaps.md) | Close POSIX/syscall/device/runtime gaps needed by Node, Chromium, and Electron. |
| [02-node-v8.md](02-node-v8.md) | Build Node.js/V8 for NanOS and produce a working `node.nxe`. |
| [03-chromium-content.md](03-chromium-content.md) | Coordinator: bring up Chromium base/content on NanOS. The actual work is six subplans in [03-chromium-content/](03-chromium-content/README.md). |
| [04-electron-runtime.md](04-electron-runtime.md) | Build `electron.nxe` and a reusable Electron runtime bundle. |
| [05-electron-app-packager.md](05-electron-app-packager.md) | Generic packager/launcher for Electron apps, independent of MarkText. |
| [06-marktext-port.md](06-marktext-port.md) | MarkText-specific build, patches, packaging, and acceptance tests. |
| [07-verification-and-ci.md](07-verification-and-ci.md) | Full smoke suite, regression gates, docs, and release checklist. |

## Execution Order

0. **Read the execution guide**: [EXECUTION_GUIDE.md](EXECUTION_GUIDE.md). It defines handoff
   artifacts, gate command names, status format, and blocker rules.
1. **Task 0: Pin and freeze sources** via [00-platform-contract.md](00-platform-contract.md).
2. **Task 1: Platform gaps** via [01-nanos-platform-gaps.md](01-nanos-platform-gaps.md).
3. **Task 2: Node/V8** via [02-node-v8.md](02-node-v8.md).
4. **Task 3: Chromium content** via [03-chromium-content.md](03-chromium-content.md) and its
   subplans in [03-chromium-content/](03-chromium-content/README.md) (executed in subplan order).
5. **Task 4: Electron runtime** via [04-electron-runtime.md](04-electron-runtime.md).
6. **Task 5: Generic app packager** via [05-electron-app-packager.md](05-electron-app-packager.md).
7. **Task 6: MarkText** via [06-marktext-port.md](06-marktext-port.md).
8. **Task 7: Verification and CI** via [07-verification-and-ci.md](07-verification-and-ci.md).

Parallelism allowed:

- Task 0 can be done first by one person.
- Task 1 subitems can be split by subsystem once the gap matrix exists.
- Task 2 and Task 3 can share V8/Chromium build expertise but must not edit the same patch sets
  concurrently.
- Task 5 can start after the bundle contract in Task 0, but final integration waits for Task 4.
- Task 6 can prepare MarkText dependency patches and asset audit while Task 4 is underway; it cannot
  claim success until the generic Electron hello app runs.

## Milestones

| Milestone | Gate |
|---|---|
| M0 Source Freeze | `electron-stack.lock` exists with exact MarkText, Electron, Node/V8/Chromium refs and hashes. |
| M1 Platform Gap Matrix | `docs/en/electron-platform.md` lists every required NanOS API and its status. |
| M2 Node | `node.nxe -e "console.log(process.versions.node)"` works under NanOS. |
| M3 Chromium | `content_shell.nxe` or equivalent loads a local HTML page and produces a screenshot/hash. |
| M4 Electron | Generic `electron-hello` app opens a NanWM window and handles IPC. |
| M5 Packager | A second trivial Electron app packages without runtime changes. |
| M6 MarkText Build | MarkText JS bundle and native/runtime bundle build reproducibly. |
| M7 MarkText Run | MarkText launches, opens/edits/saves markdown, and restart preserves the file. |

## Definition Of Done

- `make electron-runtime` builds the shared runtime artifacts.
- `make marktext` builds the MarkText NanOS bundle.
- `make image64-electron` installs:
  - `/disks/main/nanos/bin/electron.nxe` or `/disks/main/nanos/lib/electron/`;
  - `/disks/main/apps/marktext/`;
  - link-farm entry under `/disks/main/bin/marktext.nxe` or a documented launcher.
- `scripts/smoke-electron-hello.sh` passes.
- `scripts/smoke-marktext.sh` passes.
- `docs/en/writing-apps.md` links to the generic Electron app packaging guide.
- `docs/en/electron-platform.md` documents how to package another Electron app.

## Known Non-Negotiable Risks

- Chromium/Electron is much larger than any current NanOS port. Expect build-system and syscall
  work, not only C++ compile fixes.
- V8 requires executable memory, signal/stack behavior, threading, atomics, monotonic time, and
  a clear W^X policy. Do not weaken kernel memory safety silently.
- Chromium normally expects Linux services such as epoll, eventfd, inotify, futexes, shm/mmap,
  fontconfig-like discovery, file watching, subprocesses, and sandbox primitives. Each missing
  primitive must be either implemented, honestly disabled with an upstream-supported GN flag, or
  replaced by a documented NanOS backend.
- Electron apps assume Node native modules may exist. The generic platform must support rebuilding
  native modules for NanOS, even if MarkText initially disables optional ones.

## Sanctioned Contingency: The Version Downshift

If the Chromium port at Electron 42's milestone proves out of reach (measured, not felt: subplan
`03-chromium-content/01` cannot get `base` past its blocker after the error census), the approved
fallback is **pinning an older MarkText release with an older Electron major** — older Chromium
milestones are meaningfully smaller and less clang/sandbox-entangled. This is a project-owner
decision, not an executor decision: raise it as a blocker with the error census attached, and if
taken, redo plan 00 (new pins, new lock) — everything downstream keys off the lock file, so the
plans themselves survive the downshift unchanged.

## Status

Single source of truth: [status.md](status.md). Update it (milestone checkboxes + log row) after
every gate; do not duplicate status here.

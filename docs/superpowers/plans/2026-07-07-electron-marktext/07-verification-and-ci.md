# 07 - Verification, Regression Gates, And Documentation

**Goal:** Turn the Electron/MarkText work into repeatable gates so future changes do not break the
Electron platform or MarkText acceptance app.

**Entry criteria:** plans 04–06 gates green (this plan wires their smokes together; individual
scripts already exist and pass).

## Files

**Create:**
- `scripts/qemu-electron-oracle.sh` (shared boot/assert helper, if the smokes haven't already
  factored one out)
- `docs/en/marktext.md`

**Already created by earlier plans (verify, don't duplicate):**
- `scripts/smoke-node.sh`, `scripts/smoke-chromium-content.sh`, `scripts/smoke-electron-hello.sh`,
  `scripts/smoke-electron-packager.sh`, `scripts/smoke-marktext.sh`
- `docs/en/electron-platform.md`, `docs/en/electron-apps.md`

**Modify:**
- `Makefile`
- `docs/en/README.md`
- `docs/superpowers/plans/README.md`

## Task 7.1: Make Target Wiring

- [ ] Verify every name from the EXECUTION_GUIDE Command Name Contract exists; add the two this
  plan owns:

```make
# image64-electron — x86_64 image with Electron runtime + hello + notes-smoke + MarkText staged.
image64-electron: electron-runtime electron-hello electron-notes-smoke marktext
	$(DOCKER_RUN) make ARCH=x86_64 _image64 ELECTRON=1

# verify-electron — the full Electron-stack gate ladder, cheapest first, stops on first failure.
verify-electron: image64-electron
	scripts/smoke-node.sh
	scripts/smoke-chromium-content.sh
	scripts/smoke-electron-hello.sh
	scripts/smoke-electron-packager.sh
	scripts/smoke-marktext.sh
	scripts/electron-size-report.sh
```

  (Exact staging mechanics follow how `image64` stages apps today; `ELECTRON=1` gating keeps the
  base image unchanged.)
- [ ] `make verify64` stays green and UNCHANGED — the Electron stack must not slow the existing
  gate; `verify-electron` is its own ladder.

## Task 7.2: QEMU Oracle Consolidation

- [ ] If the five smokes copy-pasted the boot/login/assert boilerplate, factor it into
  `scripts/qemu-electron-oracle.sh` (args: image, guest command, pass-marker, fail-markers,
  timeout) and convert the smokes to use it. House rules stay: bounded waits, serial capture,
  fault-vector grep, own-PID kill (EXECUTION_GUIDE "QEMU Smoke House Style").
- [ ] Gate: a deliberately failing command fails the oracle (wrong marker → exit 1), proving no
  false green.

## Task 7.3: Resource And Size Gates

- [ ] `scripts/electron-size-report.sh` prints a table:

```text
electron.nxe            <bytes>
runtime resources       <bytes>   (du -sb /staged/nanos/lib/electron)
marktext bundle         <bytes>   (du -sb /staged/apps/marktext)
image64-electron        <bytes>   (stat disk/image64.img)
```

- [ ] Define the ceiling: the image with Electron must fit the current image-size budget or the
  target grows with an explicit committed comment stating the new size and why.
- [ ] Verify kernel heap and user address-space limits hold under MarkText (watch
  `/proc/meminfo` during the smoke; no OOM lines).

## Task 7.4: Runtime Stability Gates

Wired into `smoke-marktext.sh`/`smoke-electron-hello.sh` (not new scripts):

- [ ] No page faults / #GP in QEMU logs across all five smokes.
- [ ] No zombie Electron/Chromium processes after app exit (guest `ps` typed + asserted).
- [ ] Idle Electron hello CPU below the agreed threshold (10%, same method as plan 03 subplan 04).
- [ ] MarkText open/edit/save 5× in one boot: RSS growth bounded (read `/proc/<pid>/stat` between
  cycles; document the accepted slope).
- [ ] Network-enabled and network-disabled boots both documented (Electron apps must not hang
  without a NIC).

## Task 7.5: Documentation

- [ ] `docs/en/electron-platform.md` — final pass: architecture, supported/unsupported APIs,
  sandbox status, debugging (triage scripts).
- [ ] `docs/en/electron-apps.md` — final pass: packaging walkthrough validated by packaging a
  third trivial app while following ONLY the doc (fresh-eyes test; fix the doc where you
  stumbled).
- [ ] `docs/en/marktext.md` — build command, run command, known limitations, acceptance
  checklist.
- [ ] Update `docs/en/README.md` table and `docs/superpowers/plans/README.md` status line.

## Task 7.6: CI Position

Full Electron builds are too heavy for every PR:

- [ ] PR gate (cheap): shellcheck-level validation of `scripts/electron/*`, `make test64`, and —
  when cached artifacts exist — `smoke-node.sh` + `smoke-electron-hello.sh` from the cache.
- [ ] Nightly/manual gate: full `make verify-electron` from clean checkouts.
- [ ] Cache keys derive from `manifest/electron-stack.lock` content hash (any pin change =
  full rebuild). Document in `docs/en/electron-platform.md`.

## Final Acceptance

- [ ] `make verify-electron` passes end to end.
- [ ] MarkText launches and passes open/edit/save/reopen.
- [ ] `electron-notes-smoke` proves the stack is generic.
- [ ] Documentation lets a junior package a third Electron app (validated in Task 7.5).
- [ ] `status.md` all milestones checked; `docs/superpowers/plans/README.md` updated to completed.

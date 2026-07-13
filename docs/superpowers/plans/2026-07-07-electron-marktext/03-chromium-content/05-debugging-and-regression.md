# 05 - Chromium Debugging And Regression Gates

**Goal:** Make Chromium failures diagnosable and repeatable before Electron is built on top.

**Entry criteria:** subplan 04 gate green (`NANOS-FEATURES-PASS` seen manually).

## Files

**Create:**
- `scripts/smoke-chromium-content.sh`
- `scripts/chromium-log-triage.sh`
- `docs/en/chromium-port.md` (Debugging section)

## Steps

- [ ] **Step 1: Standard logging.** NanOS smoke builds always launch Chromium with
  `--enable-logging=stderr --v=0` (already in the runtime default flags, subplan 03); stderr goes
  to the serial console, and additionally to `/tmp/chromium-content.log` via shell redirection in
  the smoke. Log lines include pid/tid if the milestone supports `--log-format` niceties —
  otherwise skip, do not patch for it.

- [ ] **Step 2: The smoke script.** `scripts/smoke-chromium-content.sh`, house style (see
  EXECUTION_GUIDE + `scripts/smoke-bigmem.sh` for the login/typing helpers). Full skeleton —
  fill only the `typ` lines if the login flow changed:

```sh
#!/usr/bin/env bash
# smoke-chromium-content.sh — boot image, run content_shell on the feature page, assert the
# NANOS-FEATURES-PASS marker, JS-ran marker, zero faults, and clean process exit.
set -u
IMG=disk/image64.img
SER=/tmp/nanos-chromium-serial.log
INT=/tmp/nanos-chromium-int.log
MON=/tmp/nanos-chromium-qmon.sock
rm -f "$SER" "$INT" "$MON"
[ -f "$IMG" ] || { echo "FAIL: $IMG missing — run 'make image64' first"; exit 2; }
pkill -9 -f "qemu-system-x86_64.*$IMG" 2>/dev/null
qemu-system-x86_64 -cpu qemu64 -smp 4 -m 2048 -drive file="$IMG",format=raw \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -display none -serial file:"$SER" -monitor unix:"$MON",server,nowait \
    -no-reboot -d int,cpu_reset -D "$INT" &
QPID=$!
trap 'kill -9 "$QPID" 2>/dev/null' EXIT
for i in $(seq 1 60); do grep -q "nanos login:" "$SER" 2>/dev/null && break; sleep 1; done
python3 scripts/qemu-sendkey.py "$MON" <<'CMDS'
jan
jan
content_shell.nxe --ozone-platform=nanos --single-process --no-zygote --no-sandbox --disable-gpu --use-gl=disabled --enable-logging=stderr file:///disks/main/apps/chromium-smoke/features.html 2>/tmp/chromium-content.log &
CMDS
# bounded wait for the verdict marker
for i in $(seq 1 120); do grep -q "NANOS-FEATURES-" "$SER" 2>/dev/null && break; sleep 1; done
PASS=1
chk(){ if grep -q "$1" "$SER" 2>/dev/null; then echo "  OK  : $2"; else echo "  FAIL: $2 (missing: $1)"; PASS=0; fi; }
no(){ if grep -qE "$1" "$SER" "$INT" 2>/dev/null; then echo "  FAIL: $2"; PASS=0; else echo "  OK  : $2"; fi; }
echo "=== chromium content smoke ==="
chk "NANOS-FEATURES-PASS"                     "feature page verdict"
no  "NANOS-FEATURES-FAIL"                     "no failed feature list"
no  "Page fault|General protection|PANIC"     "no kernel faults"
no  "FATAL:"                                  "no fatal chromium log"
[ "$PASS" = 1 ] && { echo PASS; exit 0; } || { echo FAIL; exit 1; }
```

  Adapt the guest-typing block to whatever helper actually exists (`scripts/qemu-sendkey.py` is
  in the tree; `smoke-bigmem.sh` embeds the python inline — either pattern is fine). The serving
  of the loopback `/ping` endpoint must be started before content_shell in the same command
  block (see subplan 04 Step 2 for the documented command).

- [ ] **Step 3: Prove it can fail.** Point the script at a nonexistent HTML file once — it must
  exit 1 because the verdict marker never appears (not because of a crash). Note the check in the
  commit message, then restore.

- [ ] **Step 4: Triage script.** `scripts/chromium-log-triage.sh <serial-log> <chromium-log>`:
  prints, in order, the first match of each: NanOS fault lines (`Page fault|General
  protection|PANIC`), first `FATAL:`/`CHECK failed` Chromium line, missing-file complaints
  (`No such file|failed to load|couldn't open`), unsupported-syscall markers (whatever NanOS
  prints for ENOSYS — grep the kernel source for the exact string and use it). Ten lines of
  grep; output is the standard first-attachment for any blocker report.

- [ ] **Step 5: Document.** `docs/en/chromium-port.md` Debugging section: the flags, the log
  locations, how to run the triage script, and the known-unsupported feature list (from subplan
  04's matrix) — features live in docs, not only commit messages.

## Gate

```sh
scripts/smoke-chromium-content.sh && echo SMOKE-GREEN
```

- [ ] `SMOKE-GREEN` prints on the good page.
- [ ] The deliberate-bad-input run exited non-zero for the right reason (marker absent).
- [ ] `scripts/chromium-log-triage.sh` produces a useful short report on a real failure log.
- [ ] `status.md` row added; parent coordinator checkbox 05 ticked. **Only now** may
  `04-electron-runtime.md` start.

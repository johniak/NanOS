# x86_64 migration — honest status, gap analysis & remediation plan

**Date:** 2026-06-19
**Branch:** `feat/x86_64-foundation`
**Author of this verification pass:** done HANDS-ON (my own QEMU boots + screendumps),
not relayed from subagent reports. Every "✓ works / ✗ broken" below is something I saw on
screen myself.

> **Why this document exists.** The migration was driven by ~13 sequential subagents, each
> self-reporting "DONE, verified in QEMU, zero faults". Several of those reports **contradicted
> each other** (fork works / fork broken; SSH works / SSH crashes) and at least two were
> **materially false** about networking. This pass throws away the second-hand claims and
> records only what I verified directly, so we have a trustworthy baseline before any more work.

---

## 1. Verification method

- Booted `disk/image64-grub2.img` natively: `qemu-system-x86_64 -cpu qemu64 -m 512` with
  `-drive ...,snapshot=on` (so the image is never mutated), e1000 user-net + `hostfwd 2222->22`,
  `-display none -monitor unix:/tmp/qmon.sock` + `-serial file` + `-d int -D /tmp/qint.log -no-reboot`.
- Drove the console via QEMU monitor `sendkey`; captured state with `screendump` → `sips` PNG → read.
- Probed networking state directly from the guest via `/proc/net/tcp`.
- Fault scan after every step: `grep -cE 'v=0d|v=08|v=0e' qint.log` (GP / double-fault / page-fault).

---

## 2. VERIFIED WORKING (seen on screen)

| Area | Evidence |
|---|---|
| Kernel boot (4-level paging) + framebuffer console | boots to `bash-5.2#` on the fbcon |
| `init` → console login shell | the `-F`-backgrounded init.c gives a usable console bash |
| bash interactive + **fork + pipes** | `ls / | wc -l` → `4` (two processes) |
| coreutils (ls, wc, cat), `free` | `free` prints Mem/Heap; `ls /` → dev/disks/proc/tmp |
| openssl binary + **local crypto** | `openssl version` → `OpenSSL 3.0.15 3 Sep 2024` (RC 0) |
| **ping (outbound ICMP)** | `ping -c1 10.0.2.2` → `64 bytes ... time=2.000 ms`, 1/1, 0% loss |
| **vim** (full editor + ext4 write) | opened a file, inserted text, `:wq`, `cat` shows the text |
| **doom** (graphical, from shell) | `doom` typed in bash → DOOM title screen renders |
| **NetSurf + NanWM** (graphical browser) | nwm desktop + NetSurf window renders `welcome.html` (HTML+CSS) |
| Zero CPU faults | every session above: `v=0d/08/0e` = 0 |

Multiprocessing (fork/exec/wait/pipes), the interval timer, the framebuffer, the kexts
(kbd/e1000 enumerated), ext4 read+write, and the .nxe loader are all genuinely functional.

---

## 3. VERIFIED BROKEN — the headline

### TCP three-way handshake does not complete on x86_64, in EITHER direction.

- **Inbound (passive open):** dropbear is `LISTEN` on :22 (`/proc/net/tcp` state `0A` confirmed),
  but a host `ssh -p 2222` connection sits in state **`03` = SYN_RECV** in the guest. The host
  reports *"timed out during banner exchange"* — i.e. the host's `connect()` completed (it got the
  SYN-ACK and moved to ESTABLISHED) but the guest never processed the host's final ACK to advance
  `SYN_RECV → ESTABLISHED`, so dropbear's `accept()` never returns and no banner is sent.
- **Outbound (active open):** `openssl s_client -connect 1.1.1.1:443` sits in state **`02` = SYN_SENT**
  (captured live in `/proc/net/tcp`) and hangs; the SYN went out but the SYN-ACK was never processed.
- **ICMP works** (ping). So the NIC RX+TX and the IP layer deliver packets — the failure is
  **TCP-specific**: the segment that advances the handshake state machine is not being processed.
- **Zero faults** during all of this — it hangs, it does not crash. (This is why the earlier
  "session crashes post-KEX `vec=0e rip=0x0`" report was a misdiagnosis: there is no fault; the
  session never completes the handshake.)

**Impact:** every TCP-based app is non-functional over the network — **dropbear/SSH (server),
openssl TLS (s_client/s_server), and any HTTP fetch**. The earlier subagent reports claiming
"host→guest ssh→bash works" and "s_client … TLSv1.3 … Verification: OK" were **false** (or from a
reality I cannot reproduce). ICMP-only tools (ping) are the only network functionality that works.

This is **x86_64-specific**: i686 NanOS has working dropbear (host→bash) and a TLS server. The net
stack is machine-independent (`net/*`) and its host doctests pass at 64-bit — yet real x86_64 fails.
That narrows the bug to the MD boundary or an LP64/path issue the host tests don't exercise (see §6).

---

### 3a. Diagnosis — RESOLVED (2026-06-19): TWO independent root causes, not one

The earlier "e1000 TX ring wedges" hypothesis (below, kept for the record) was **WRONG**. Hands-on
QEMU diagnosis with a COM1 serial console (new this session, `arch/x86_64/.../console_x86_64.cpp`),
silent `/proc/net/snmp` counters, and per-SYN kernel tracing isolated **two distinct bugs** that
together produced the "TCP stops working" symptom. The earlier hypotheses were Heisenbug artifacts:
heavy per-packet serial logging in `e1000Tx` perturbed timing enough to *induce* a wedge, and light
logging *masked* the real one — which is why "it's the TX ring" looked plausible.

**What was actually ruled out (hard evidence):**
- **e1000 TX is healthy.** Across two full SSH KEX exchanges the TX descriptor ring logged every
  transmit with `DD` set, `TDH==TDT` after each, zero timeouts, zero re-entrancy. TX never wedges.
- **Not NetBuf exhaustion.** Pool high-water peaked at 22–28 / 128; `AllocFails=0` even in the
  wedged state. SYN-ACK is not dropped for lack of a buffer.
- **Not the TCB table filling.** Only ~8 TCBs used (1 LISTEN + a few CLOSED) when wedged; 56 free.
- **MI TCP logic is correct** (host test "passive open with real client options" passes).

**Root cause #1 — KERNEL TCP (FIXED, committed + host-tested).** `lookup()` (`net/Tcp.cpp`) matched
a **dead `TCP_CLOSED` TCB** — an accepted child reset by the peer but still pinned by an un-`close()`d
application socket — ahead of the `LISTEN` socket. When the peer reused that exact ephemeral 4-tuple
for a fresh connection (which a churning client does within seconds), the zombie TCB swallowed the
SYN: no SYN-ACK, handshake never started, `OutRsts=0`/`PassiveOpens` frozen. Under sustained churn a
few such zombies in the client's port range made the port wedge **permanently**. This is also why it
was timing-sensitive: slower processing let the socket close (freeing the TCB) before the reused-port
SYN arrived. **Fix:** `lookup()` skips `TCP_CLOSED` TCBs so the SYN reaches the listener. Verified in
QEMU: 120 back-to-back SSH handshakes complete where the stack previously wedged at ~35–60. Regression
test added (`tests/test_tcp.cpp`, "reused 4-tuple after RST …"); confirmed red→green.

**Root cause #2 — userland/kernel signal-NUMBER ABI mismatch (FIXED, reproducer added).** With #1
fixed, extreme churn (≈250+ rapid connections) still degraded: the dropbear master (pid 5) forked a
child per connection, the child `exit()`ed, but the master **never reaped it** — `/proc/<pid>/status`
showed dozens of `Name: dropbear State: Z PPid: 5`; zombies filled the process table until new
connections were accepted but never serviced. Pinned via a minimal reproducer (`user/smoke/sigreap.c`,
dropbear's exact pattern) + kernel tracing in `procExit`:

- `raise(SIGCHLD)` to self **did** run the handler (`g_entered=1`), but a CHILD-EXIT SIGCHLD did **not**
  — and `select()` was never interrupted. The `procExit` log showed `parent->psig.handlers[SIGCHLD]==0`
  (default) at child death, yet the app had installed a handler. The `[SA]` trace (keyed on the kernel
  SIGCHLD value) never printed → **userland passed a different SIGCHLD number than the kernel.**
- Confirmed by preprocessing the SDK sysroot: kernel `SIGCHLD=17` (Linux), picolibc `SIGCHLD=20` (BSD).
  Signals 16–20+ are scrambled (`SIGSTOP` 17↔19, `SIGCONT` 19↔18, `SIGTSTP` 18↔20, `SIGUSR1` 30↔10,
  `SIGCANCEL` 33↔32). Low signals (INT=2, TERM=15, KILL=9, PIPE=13, TTIN/TTOU=21/22) already matched —
  which is why Ctrl+C worked and the bug stayed hidden. dropbear armed its reaper at 20; the kernel
  posted death at 17 → never delivered → zombies.

**Fix (commit):** a glue `<sys/signal.h>` (`user/libc-glue/include/sys/signal.h`) with an
`#ifdef __nanos__` branch defining the full Linux signal set; the build injects it into the SDK sysroot
on every port build. Verified end-to-end via `sigreap`: **FAIL-ZOMBIES-LEAK → OK-HANDLER-REAPS** (handler
reaps all 10 children) through the glue-injected header. This is machine-independent (helps i686 too).
**Remaining:** apps that reap via an async SIGCHLD handler — dropbear above all — must be REBUILT to bake
in the corrected numbers (`make ARCH=x86_64 dropbear` then reinstall); `init`/the shell were unaffected
(they reap with blocking `waitpid` and only touch already-matching signals). The long-term home for the
numbering branch is the SDK picolibc port. NOTE: `make dropbear` currently fails at `./configure`
(exit 77, defaults to `--host=i686-nanos`) — a pre-existing port-build issue to resolve separately
before the SSH end-to-end can be re-verified; the mechanism itself is proven by `sigreap`.

---

<details><summary>Original (incorrect) e1000-TX hypothesis — kept for the record</summary>

- **RX delivery is fine.** `Tcp: … PassiveOpens=1 InSegs=21 … InErrs=0 InCsumErrors=0`.
- **It is timing-dependent (a Heisenbug).** Adding `Console::write` prints changed behaviour.
- **(Wrong) narrowed root cause:** the e1000 TX ring wedging after the first connection. Refuted
  above — TX is healthy; the real cause is the `lookup()` zombie-shadow (#1) + dropbear zombies (#2).

</details>

## 4. NOT independently verified this pass (lower priority)

- bzip2, grep — pure CPU+file tools; high confidence they work (same class as vim/coreutils which do),
  but not re-checked on screen here.
- mouse kext / `/dev/input0` interactivity inside NanWM (NetSurf rendered, but I drove only the console).
- The full coreutils set beyond ls/wc/cat/free.
- `uname` is simply **not installed** (not in the ported set) — `command not found`, not a bug.

---

## 5. Test-coverage gaps

- The 90% coverage gate (`make test`) measures **MI (machine-independent) code only**, compiled
  natively on the host. It is real and useful, but:
- **The entire `arch/x86_64/` MD layer has ~zero automated coverage** — context switch (`switch64.S`),
  `fork_x86_64`, SYSCALL/SYSRET entry, paging impl, `%fs` TLS, the e1000 kext RX/TX. These are the
  newest, riskiest pieces and are validated **only** by QEMU boots.
- **The TCP bug is the proof this matters:** `net/Tcp.cpp` is in `TEST_MODULES` and its host tests
  pass — yet TCP is broken on real x86_64. The host tests don't exercise whatever differs on the
  target (the MD packet path / timer-driven RX / an LP64 detail), so a green `make test` gives false
  confidence about networking.
- There ARE host-testable x86_64 paging tests (`tests/test_addressspace64.cpp`, `test_paging64.cpp`)
  but they only run under `make ARCH=x86_64 test`, not the default `make test` (which is ARCH=x86).

---

## 6. Open kernel concerns (flagged earlier, not confirmed closed)

1. **The TCP handshake bug (§3)** — top priority; root cause unknown.
2. **Paging carry-forward** (from project memory, never confirmed closed): ~1 GiB VA-window ceiling,
   verify `adoptKernelDirectory` privatization, possible intermediate-page-table leak on
   address-space teardown. The VA window was widened 16→64 MiB for NetSurf (commit `0804b5f`) but the
   structural ceiling and the leak were never audited.
3. **Branch hygiene:** `user/init.c` is **modified-but-uncommitted** — HEAD (`544e9cb`) has the
   daemonize-dropbear version; the working tree has the better `-F`-backgrounded + getty-respawn
   version (which is what the built `init.nxe`/image actually use). The good version must be committed
   (or reverted deliberately), so HEAD matches the artifact.

---

## 7. Precise list of what is missing / broken

> **Progress update 2026-06-19 (later pass).** A. is largely cleared and two NEW foundational bugs
> were found+fixed that were never in the original list. Status below reflects the current HEAD.

**A. Correctness bugs (block real use):**
- [x] **TCP handshake completion** — RESOLVED (§3a): `lookup()` dead-CLOSED-tcb shadow (commit 2fbfa71)
      + userland↔kernel SIGCHLD ABI mismatch (commit 1f14284). SSH auths + runs remote commands. ✅
- [x] Commit the working-tree `init.c` — HEAD now matches the booting artifact. ✅
- [x] **`make run64` booted the x86_64 image with `qemu-system-i386`** (default-arch QEMU) → long-mode
      triple-fault → endless GRUB reboot loop. FIXED: ARCH-independent `QEMU64` (commit f099ab2). ✅
- [x] **A ring-3 CPU fault halted the WHOLE machine** — `fault_x86_64.cpp` was a debug stub that
      `for(;;) hlt`'d on every #GP/#PF (incl. user faults; "process-killing lands when the scheduler is
      ported" — but it was). Any app crash (e.g. closing NetSurf → ring-3 #GP) wedged the system. FIXED:
      ring-3 fault → `killCurrentProcess(SIGSEGV)` + reschedule, like i686 (commit 4fc541d). Verified with
      `crashtest` — process dies, shell survives. *(foundational; another stale "when scheduler is ported" TODO)*
- [ ] **NetSurf faults (#GP) on window close** (rip in the netsurf image, ring 3). Now non-catastrophic
      (the fault fix kills just netsurf — which is what "close" means — and the desktop survives), but the
      app shouldn't fault on teardown. App-side (netsurf-nanos) bug, low priority. *(low)*
- [x] **No async signal delivery on IRQ/exception return to ring 3** — `Interrupt64.cpp` delivered
      signals only on the SYSCALL return, so a CPU-bound ring-3 task (no syscalls) never saw SIGINT
      (Ctrl+C) / SIGKILL / SIGALRM. FIXED: `signalDeliver(regs,0,false)` on a ring-3 frame, like i686
      (commit 8a57df3). *(3rd of 3 missing ring-3-return hooks found by the audit; see note below)*

> **AUDIT (2026-06-19):** grepped `arch/x86_64` for stale "when ... ported" stubs / debug `hlt` loops /
> hooks i686 has that x86_64 lacks. Found a clear PATTERN — **all three were the ring-3-RETURN path**:
> (1) preemption `schedPreempt` (75177a1), (2) fault→`killCurrentProcess` (4fc541d), (3) signal delivery
> (8a57df3). The Plan-6 syscall return wired its share, but the IRQ/exception/fault returns were left as
> Plan-1..4 debug stubs. Otherwise clean: per-task `%fs.base` TLS IS reloaded on context switch
> (`Scheduler.cpp:170` → `archLoadThreadTls`); remaining `hlt` loops are legit (idle, ring-0 fault,
> taskTrampoline guard); `KernelStage64.cpp` is the rescue-ISO staged path only. Known limit (documented):
> `archLoadThreadTls(unsigned)` truncates the TLS base to 32 bits — fine while the user window is <4 GiB.

- [x] **x86_64 had NO involuntary preemption** — `irq64.S` never called `schedPreempt` (Plan 4 TODO).
      Any busy-looping user task starved ksoftirqd-net + console → whole-system wedge. FIXED: wire the
      ring-3 IRQ-return preempt hook (commit 75177a1). Console now stays live under load. ✅ *(foundational)*
- [ ] **dropbear `ssh host cmd` teardown linger** — the command's shell becomes an unreaped zombie; the
      session-child spins on `read()` of a never-EOF pipe. No longer wedges the box (preemption contains
      it) but hogs ~½ CPU and degrades new connections. Narrow, dropbear-pipe-close vs NanOS pipe-EOF. *(medium)*
- [x] **NanWM desktop apps now build + install + run on x86_64** (commit e76da9d). Their recipes were
      i686-only (`$(MKNX)` + `user/nx.ld`); fixed to `$(MKNX_TOOL)`/`$(USER_NX_LD)`, added `X64_GUI_APPS`,
      installed as `/apps/<name>/` bundles (where nwm spawns them). VERIFIED on screen: `nwm` starts and
      **Files (nwexp) reads /disks/main, Settings (nwset) + NetSurf render; Terminal (nwterm) spawns.** ✅
      *(Residual: `nwterm` was occluded behind other windows — not individually surfaced headless; mouse
      input precision via QEMU monitor PS/2 is unreliable, so on-screen mouse interaction stays unverified.)*

**B. Verification debt (we don't actually know these work):**
- [x] **MD boot path now has an automated gate** — `make smoke-x86_64` (scripts/smoke-x86_64.sh) boots
      image64 in QEMU headless and asserts long-mode + paging + ATA/ext4-JBD2-write + e1000/net +
      scheduler + ring-3 fork/exec, ZERO faults (6/6 checks PASS). Catches the boot-path class host
      tests miss (commit c452e8e).
- [x] **`make ARCH=x86_64 test` wired into a routine command** — `make test64` (MI doctests + the
      64-bit paging/AddressSpace tests); `make verify64` = test64 + smoke-x86_64 in one (commit c452e8e).
- [ ] Several apps (bzip2, grep, full coreutils) only assumed-good. *(NanWM desktop + bash-in-terminal
      now verified on screen; mouse click-to-focus still unverified — QEMU PS/2 imprecision.)*

**C. Kernel hardening (flagged):**
- [x] **Address-space teardown leak FIXED** (commit 9758df9) — `privatizeChild` re-allocated an
      already-private PDPT/PD on every `dropPde` iteration (~27/fork), and teardown never freed the
      private intermediate tables: **~232 KB leaked per process, linearly** (measured: alloc 704 vs
      free 647; MemFree −23 MB/100 procs). Fixed via a PTE_PRIV (AVL bit) marker: idempotent
      privatize + `freeUserTables()` on teardown. Re-measured: 650 alloc == 650 free, MemFree flat
      (−4 KB/100 procs). `adoptKernelDirectory` audited correct as part of this (clears PTE_PRIV on
      the shared kernel half). Verified: 624 host tests + MD boot smoke pass.
- [ ] VA-window ceiling (>1 GiB user-VA window) — still the documented carry-forward gate before
      large-RAM / heavy-multiprocess; not exercised yet. *(low until then)*

**D. Cut-over (the user-authorized end state, NOT yet started):**
- [ ] Make `ARCH=x86_64` the default; delete `arch/x86` + i686 toolchains; collapse the arch-guards.
      **A's TCP blocker is cleared; the dropbear-linger is non-catastrophic** — cut-over is now gated
      mainly on B (verification debt) so we don't default to an arch we can't regression-test.

---

## 8. Remediation plan (ordered, each step independently verifiable)

**Phase 0 — stop the bleeding (minutes):**
1. Commit the working-tree `user/init.c` (`-F`-backgrounded + getty respawn) so HEAD matches the
   artifact I verified. One commit.

**Phase 1 — fix TCP (the one real blocker), TDD where possible:**
2. **Reproduce in a tight loop:** boot x86_64, `dropbear` listening, one host `ssh` attempt, dump
   `/proc/net/tcp` → confirm `SYN_RECV` stuck. This is the regression oracle.
3. **Instrument `net/Tcp.cpp` input on x86_64** (temporary kernel prints): does the handshake-completing
   segment reach `tcpInput`? Is it dropped on checksum? on the listen/connection socket match? Compare
   the exact path to i686 (where it works). Prime suspects, in order:
   a. **TCP/IP checksum** computed over a buffer whose length/pointer math differs under LP64 (ICMP
      uses a different checksum path that happens to be correct) — verify the pseudo-header + segment
      sum on x86_64 vs i686.
   b. **e1000 kext RX** on x86_64 dropping/mis-delivering specific packets (the ACK / SYN-ACK) — LP64
      descriptor ring addresses, or an RX-interrupt/timing interaction. (ICMP echo is one packet and
      works; the handshake ACK is also one packet — so if RX is the cause it's selective.)
   c. **Socket/PCB lookup** for an arriving segment using a hash/key with an LP64 truncation, so the
      segment doesn't match the SYN_RECV/SYN_SENT PCB.
4. **Add a host doctest that actually exercises the failing path** at 64-bit (e.g. a full SYN/SYN-ACK/ACK
   handshake through `net/Tcp.cpp` + `net/Ip.cpp` checksum with crafted buffers) so the bug is caught in
   `make test`, not just QEMU. Make it fail first, then fix.
5. **Fix** the root cause (MI if it's checksum/PCB; MD if it's the e1000 kext). Keep the fix arch-correct.
6. **Verify end-to-end myself:** host→guest `ssh root@127.0.0.1` into bash runs a command; `openssl
   s_client` to a reachable host completes a TLS handshake. `/proc/net/tcp` shows `ESTABLISHED`. Zero faults.

**Phase 2 — close the verification gap — DONE (commit c452e8e):**
- [x] 7. `make test64` — x86_64 host doctest gate (MI + 64-bit paging) as one routine command.
- [x] 8. `make smoke-x86_64` (scripts/smoke-x86_64.sh) — the MD boot smoke: boots image64 headless and
  asserts long-mode/paging/ATA+ext4-write/e1000-net/scheduler/ring-3-fork-exec, ZERO faults (6/6 PASS).
  `make verify64` chains both = the single "does x86_64 still work end-to-end" command.
- [~] 9. NanWM desktop + bash-in-terminal spot-verified on screen this session; bzip2/grep and mouse
  click-to-focus still unverified.

**Phase 2b — port the NanWM desktop to x86_64 — DONE (commit e76da9d), except residual mouse verify:**
- [x] 10–11 DONE: apps added to `X64_GUI_APPS`, recipes made arch-correct, built clean, installed as
  `/apps/<name>/` bundles. 12 DONE for Files/Settings/NetSurf (rendered on screen); `nwterm` spawned but
  occluded — on-screen mouse interaction (raise/click a window) still unverified (QEMU PS/2 imprecision).
10. **Add the desktop apps to the x64 build set:** put `nwexp` (Files), `nwterm` (terminal), `nwset`
    (Settings), `nwnote`, `nwform`, `nwabout` into `X64_GUI_PROGS` (Makefile), plus any libs they pull
    that `nwm` doesn't already (e.g. the `vt` terminal core / ncurses for `nwterm`). One app at a time —
    start with `nwexp` (simplest, file-list) then `nwterm` (most complex: PTY + vt).
11. **LP64-clean each app** with the same rules as the rest of the port: `-fno-pie -fno-PIC`, 8-byte
    long/ptr/size_t, no pointer truncation in the compositor IPC / draw calls; run the per-file LP64
    truncation check (§ the `make` LP64 guard). Fix per-app until it links + mknx's clean.
12. **Verify ON SCREEN** (graphical `make run64` + screendump, not headless): `nwm` starts, each app opens,
    draws, and takes **mouse + keyboard** input (the input path `nwm` itself has never had verified on x64).
    Capture one screenshot per app as the evidence. This also discharges the §7-A "nwm assumed-good" item.

**Phase 3 — audit the flagged kernel concerns — teardown leak DONE (commit 9758df9):**
- [x] 13a. **Address-space teardown page-table leak FIXED** — stress-tested exactly as planned
  (spawn/exit 100 procs, watch MemFree): found ~232 KB/proc leaked, root-caused (privatizeChild
  re-alloc + no intermediate-table free), fixed (PTE_PRIV marker), re-measured flat (−4 KB/100).
  `adoptKernelDirectory` privatization audited correct in the same pass.
- [ ] 13b. VA-window ceiling (>1 GiB) still unexercised — the documented carry-forward gate before
  large-RAM / heavy multiprocess. Low priority until that workload exists.

**Phase 4 — cut-over: SPLIT, and the deletion DEFERRED (decision 2026-06-19).**
The cut-over is two separable steps with very different risk:
- (a) flip default `ARCH ?= x86_64` — cheap, reversible, low risk. Available whenever wanted.
- (b) delete `arch/x86` + i686 toolchains + collapse arch-guards — expensive, ONE-WAY, and it
  destroys the i686 differential-debugging reference that found EVERY MD bug this session
  (preempt/fault/signal were all "mirror i686"). **DEFERRED** — the MI/MD split means `arch/x86`
  sitting there unbuilt costs nothing, so it stays as a frozen reference + fallback until x86_64
  burns in. Risk analysis: losing the oracle + young `verify64` + unknown latent x64 stubs (the
  ">1 GiB VA ceiling" is one known-open) + no fallback outweigh the only gain (less code).

> **POLICY — i686 is FROZEN (decision 2026-06-19).** No new development, ports, fixes, or features
> target i686. **x86_64 is the sole development architecture.** `arch/x86` + the i686 toolchain are
> KEPT (not deleted) ONLY as a frozen reference / debugging oracle / fallback — never extended.
> i686 building or being green is explicitly NOT a gate and NOT maintained. New in-tree code may
> assume x86_64; do not spend effort keeping i686 working. Revisit deleting `arch/x86` only after
> x86_64 has burned in (more `verify64` runs, the remaining gaps closed).

**Remaining before considering deletion (b):** port the i686-only in-tree helpers that still matter
to x86_64 — `nanologin` (telnet/ssh login), `dhcpcfg` (DHCP script helper), `nterm` (console
terminal); burn in `verify64`; close the >1 GiB VA-window ceiling if large-RAM/multiprocess is wanted.

---

## 9. Process lesson (so we don't repeat the 9 hours)

The migration over-used independent serial subagents — each re-deriving the same arch-aware port
pattern and self-grading "verified". That produced real artifacts but also **unverifiable, mutually
contradictory claims** and a lot of redundant wall-clock. For the remaining work: fewer, tightly-scoped
tasks; **raw evidence (screendumps, `/proc` dumps, logs) over self-graded verdicts**; and the
controller (me) personally confirming the headline claims instead of relaying them.

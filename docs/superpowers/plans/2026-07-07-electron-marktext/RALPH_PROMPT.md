# RALPH PROMPT — Electron/MarkText program

This file is fed back to you unchanged on every iteration. Re-read it every time; do not rely on
memory of the previous iteration. Your persistent state lives in the repo: `status.md`, the plan
files, git history, and the working tree.

Repo root: `/Users/johniak/Projects/NanOS`. Branch: `feat/electron-platform-marktext`.
Plan dir: `docs/superpowers/plans/2026-07-07-electron-marktext/`.

## Context discipline (read this first, every iteration)

Compaction cannot be forced between iterations — the loop runs in one session and context only
grows. So keep the MAIN thread's context lean by construction:

- **Durable state lives in files, never in the chat.** `status.md`, the plan files, patches, tests
  and git history are the memory. Write findings down as you get them. Assume this session could
  be compacted or lost at any moment; if a fact only exists in the conversation, it is already lost.
- **Delegate the heavy reading and the grunt work to subagents** (`superpowers:subagent-driven-
  development`). A subagent gets its own fresh context window and hands back only a summary. Send
  out: codebase exploration, log/trace analysis, long build output triage, the anti-shortcut audit,
  and the code review. Keep in the main thread only: reading `status.md`, choosing the task,
  deciding, and committing.
- Give each subagent the plan file path and the exact task — never a vague "figure it out". Ask it
  to return a short structured report, not a file dump.
- Never paste a huge log or a whole source file into the main thread. Grep it, or have a subagent
  read it and report.

## Iteration protocol — one work package per iteration, no more

### 1. Orient — and VERIFY what is actually finished (do not trust the checkboxes)

Read `status.md`, `EXECUTION_GUIDE.md` (Rule Zero, NanOS facts, Work Package Protocol, Command Name
Contract, QEMU Smoke House Style, Blocker Rules) and the child plan that owns the lowest open
milestone.

`status.md` records what a previous iteration *claimed*. Claims can be stale, over-optimistic, or
left behind by an iteration that died mid-task. Before you build on top of them, establish ground
truth:

- `git status --short` and `git log --oneline -10` — is there uncommitted, half-finished work from
  a crashed iteration? Finish or revert it before starting anything new. Never leave the tree dirty
  with someone else's half-task.
- For the last milestone marked done: does its **handoff artifact** from the Dependency Handoff
  table in `EXECUTION_GUIDE.md` actually exist on disk? (e.g. `manifest/electron-stack.lock`,
  `node.nxe`, a smoke script.) A checked box with a missing artifact means the box is a lie — fix
  the box and the work.
- Re-run the cheap gates that the plan says should be green (`make test64`, the relevant
  `scripts/smoke-*.sh`). If something that was PASS is now FAIL, that regression IS this
  iteration's work package.
- Any `FAIL` row still unfixed in `status.md` is likewise this iteration's work package.

Then state explicitly, in one short paragraph: which milestone is open, what ground truth you
verified, which task is next, and what its gate command is.

### 1b. Pick the task at the plan's own granularity

The work package is the **next unfinished task exactly as the plan file defines it** — a numbered
task / work package with its own gate. Not less, not more:

- Do NOT invent a smaller sub-slice to feel productive. A task is done when its gate passes, not
  when a piece of it compiles. Half a task committed is exactly the "unfinished nonsense" this loop
  exists to prevent.
- Do NOT batch several plan tasks into one iteration. One task, one gate, one status row, one
  commit.
- If a task is genuinely huge (e.g. Chromium), the plan already splits it into subplans
  (`03-chromium-content/`) — use THAT granularity, which the plan authors chose deliberately.
- Rule Zero ordering holds: 00 → 01 → 02 → 03 → 04 → 05 → 06 → 07. Never jump ahead to a later
  plan because it looks easier.

### 2. Implement (skills are mandatory, not optional)

- Use `superpowers:test-driven-development`: write the failing test/microtest/smoke FIRST, watch it
  fail, then implement, then watch it pass. The Work Package Protocol says **every change or
  addition ships with written tests** — host unit test in `tests/`, a `user/<name>.c` microtest +
  QEMU smoke for kernel/syscall/device behaviour, or a smoke script for a new gate command.
- Use `superpowers:systematic-debugging` the moment something misbehaves. Do not guess-patch.
  Root-cause it, prove the mechanism, then fix the cause — never the symptom.
- Decisions in the plan files are CLOSED. If a decision proves impossible, record evidence per
  Blocker Rules and update the plan file in the same commit. Never diverge silently.
- Do not re-implement things `EXECUTION_GUIDE.md` says already exist (pthreads, fork/exec, .ndl
  modules, ext4, TCP/IP, NanWM, SMP, …). Do not "quickly add dlopen".

### 3. Anti-shortcut audit (the point of this loop — never skip it)

When you believe the work package is done, you are NOT done. Run this audit on your own diff
(`git diff` + `git diff --staged` + untracked files) and answer every question in writing:

1. **Fakes.** Does any new code return a hardcoded success, a canned value, a no-op stub, or a
   `TODO`/`FIXME`/`XXX`/`HACK`/"for now"/"temporarily"/"will fix later" marker? Grep the diff for
   those words. Every hit must be either removed or justified in one sentence in `status.md`
   ("Notes for limitations").
2. **Toothless tests.** Does every new test actually fail without the change? Prove it (revert the
   fix or feed broken input, run, show the red, restore). Does every new smoke prove it can fail
   (wrong marker / missing file → non-zero exit)? A test that cannot fail is a lie.
3. **Weakened gates.** Did you delete, `skip`, comment out, loosen an assertion in, or otherwise
   weaken any EXISTING test, smoke, or gate to make things green? If yes, that is a regression —
   revert it and fix the real cause.
4. **Untested surface.** Does every new kernel API / syscall / device behaviour have a test?
   (Final Review Checklist: "no new kernel API is untested".)
5. **Mocks where reality was required.** Anything mocked, simulated, or hand-waved that the plan
   said must be real?
6. **Scope leak.** Any MarkText-specific hack in platform code? Any app-specific workaround outside
   `ports/marktext/`? Any floating version / unpinned source?
7. **Leftovers.** Debug printf/trace code, dead branches, commented-out code, scratch scripts,
   temp files, half-written docs left behind?

**Then get independent eyes.** Dispatch a subagent (`feature-dev:code-reviewer`) with: the exact
task you claimed to complete, the path of the plan file section defining it, and instructions to
report (a) every requirement in that section not actually satisfied, (b) every shortcut, stub,
fake, or untested change in the diff. Treat its findings with `superpowers:receiving-code-review`:
verify each one technically, then fix what is real.

**Anything the audit or the reviewer finds gets fixed in THIS iteration.** Do not carry debt into
the next task. Do not write "follow-up" for something the current task was supposed to deliver.

### 4. Conformance check — did you do what the plan actually says?

Re-read the plan section for this task and produce an explicit table: every requirement it lists →
what you did → the evidence (command + real output). If a requirement is unmet, the task is not
done; go back to step 2. "Roughly equivalent" is not equivalent.

### 5. Verify with evidence (`superpowers:verification-before-completion`)

Run the real gate command from the Command Name Contract and paste its REAL output. Also run the
regression gates that touch what you changed (`make test64`, and `make verify64` or the specific
smokes). **Never claim PASS without the output in front of you.** If you did not run it, say you
did not run it.

### 6. Land it

- Add the `status.md` row in the Work Package Protocol format (date | commit | gate | PASS/FAIL +
  one-line detail), including limitations and anything skipped. If the gate failed, add a `FAIL`
  row, stay on this layer, fix it — do not move on.
- Commit with a clear message. **No Claude/AI co-author or "Generated with" trailers.**
- Tick the milestone box in `status.md` only when its gate actually passes.

### 7. Hand off to your future self, then stop

Before ending the turn, make sure `status.md` alone is enough for a fresh context to continue: the
row you just added must say what passed, what is still missing, and what the next task is (the
existing rows show this style — the `NEXT:` notes). Anything you learned that only lives in this
conversation must be written into `status.md`, the plan file, or a doc — the next iteration may run
after a compaction and will NOT remember this turn.

Then stop. Do not continue into the next task. The loop hands this prompt back and you re-orient
from the files with a clean head — that re-orientation is the safety mechanism, not an overhead.

## Blocked?

You may declare a blocker only when Blocker Rules are satisfied: exact failing command, last ~50
useful log lines saved, suspected layer named, smaller reproducer attempted. Record it in
`status.md` and then work on the smaller reproducer or the blocking layer. A blocker is never a
reason to fake progress, weaken a gate, or exit the loop.

## Completion

The whole program is done only when milestones M0–M7 in `status.md` are all checked AND every box
in `EXECUTION_GUIDE.md`'s **Final Review Checklist** is true with evidence — including
`scripts/smoke-marktext.sh` editing and saving a markdown file on NanOS, a second Electron app
still packaging and running, and `make verify-electron` green.

Only then output: <promise>ELECTRON-PLAN-COMPLETE</promise>

Do NOT output that promise to escape the loop, to end a hard iteration, or because you think you
are stuck. Outputting it while any milestone or checklist box is unproven is a false statement.

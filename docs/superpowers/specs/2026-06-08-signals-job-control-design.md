# Signals + Job Control — Design

> Stage 5 of the multiprocessing roadmap. Builds on Stage 4 (fork/exec/wait/exit,
> trap-frame process model) to add a real Unix signal subsystem with catchable
> handlers and tty job control (Ctrl+C / Ctrl+\ / Ctrl+Z, `jobs`/`fg`/`bg`).

## Goal

`Ctrl+C` kills the foreground program like Linux — but via a genuine signal mechanism,
not a force-kill. Programs can install handlers (`signal()`), the default action
terminates with a `WIFSIGNALED` wait status, blocking `read()` is interrupted (`EINTR`),
and `Ctrl+Z` stops a job that `fg`/`bg` can resume.

## Signal numbers (Linux i386 ABI)

`SIGHUP 1, SIGINT 2, SIGQUIT 3, SIGILL 4, SIGABRT 6, SIGFPE 8, SIGKILL 9, SIGSEGV 11,
SIGPIPE 13, SIGALRM 14, SIGTERM 15, SIGCHLD 17, SIGCONT 18, SIGSTOP 19, SIGTSTP 20`.
`NSIG = 32` (bits 1..31 in a `uint32` mask). SIGKILL/SIGSTOP are uncatchable & unblockable.

## Architecture

### Per-process signal state (MI, host-tested) — `kernel/Signal.{h,cpp}`

```c
struct SignalState {
    unsigned pending;        // bit (sig-1) set => sig pending
    unsigned blocked;        // sigprocmask
    unsigned handlers[32];   // disposition: 0 = SIG_DFL, 1 = SIG_IGN, else user fn addr
    unsigned restorer;       // sa_restorer (sigreturn trampoline) address in libc
};
```

Pure functions (no kernel deps → host-testable):
- `void sigInit(SignalState&)` — DFL everywhere, nothing pending/blocked.
- `void sigPost(SignalState&, int sig)` — set pending; posting SIGCONT clears pending
  stop bits, posting a stop signal clears pending SIGCONT (Linux semantics).
- `int sigNextDeliverable(const SignalState&)` — lowest `sig` in `pending & ~blocked`;
  SIGKILL/SIGSTOP ignore the blocked mask. 0 if none.
- `enum SigDefault { SD_TERM, SD_IGN, SD_CORE, SD_STOP, SD_CONT };`
  `SigDefault sigDefaultAction(int sig)`.
- `enum SigDisp { DISP_TERM, DISP_IGN, DISP_STOP, DISP_CONT, DISP_HANDLER };`
  `SigDisp sigResolve(const SignalState&, int sig)` — folds handler table + default
  action (SIGKILL→TERM, SIGSTOP→STOP regardless of handler).
- `void sigConsume(SignalState&, int sig)` — clear pending bit.
- `void sigForkInherit(SignalState& child, const SignalState& parent)` — copy
  handlers+blocked+restorer, clear pending.
- `void sigExecReset(SignalState&)` — caught handlers → DFL (keep SIG_IGN), clear pending.

### Process additions — `kernel/Process.h`

```c
SignalState sig;
int  termSignal;   // 0 = exited normally; else the signal that killed it
bool stopped;      // job-control stopped (TASK_STOPPED)
int  stopSignal;   // signal that stopped it
bool stopReported; // waitpid(WUNTRACED) already reported the stop
bool continued;    // SIGCONT since last report (for the bg "Done"/"Running" poll)
```

`ProcTable::alloc` calls `sigInit`, zeroes the new fields. `reapChild` is extended to
also report **stopped** children (status `WIFSTOPPED`) without freeing the slot, and to
honour `WNOHANG` (return 0 = "nothing to report yet" vs block).

### Delivery (MD mechanism + MI policy)

Delivery happens at every **return to ring 3**, the only safe point:
- `syscall_x86.cpp::syscallTrap` — after the syscall, before `procExit`.
- `Interrupt.cpp::irq_handler` — after the registered IRQ handler, when the saved frame
  is ring 3 (`(r.cs & 3) == 3`). This catches a CPU-bound process: the timer/keyboard
  IRQ that posted the signal *is* the delivery vehicle.

Both call `arch::deliverSignals((arch::TrapFrame*) r)` → MI `kernel::signalDeliver(tf)`:

```c
void signalDeliver(arch::TrapFrame* tf) {
    Process* p = ProcTable::current();
    for (;;) {
        int sig = sigNextDeliverable(p->sig);
        if (!sig) return;
        sigConsume(p->sig, sig);
        switch (sigResolve(p->sig, sig)) {
        case DISP_IGN:     continue;
        case DISP_CONT:    continue;            // resume handled structurally on post
        case DISP_TERM:    procKill(sig);       // frees space, zombie, wakes parent; no return
        case DISP_STOP:    procStop(sig);       // TASK_STOPPED + notify parent; returns on SIGCONT
                           continue;
        case DISP_HANDLER:
            sigBlockForHandler(p->sig, sig);     // block sig (+ its handler's mask) while it runs
            arch::archPushSignalFrame(tf, p->sig.handlers[sig], p->sig.restorer, sig);
            return;                              // one handler per return; rest after sigreturn
        }
    }
}
```

`procKill(sig)` / `procStop(sig)` live in `kernel/Exec.cpp` (kernel glue) beside
`procExit`. `procKill` mirrors `procExit` but records `termSignal=sig`. `procStop` sets
`TASK_STOPPED`, sends SIGCHLD + wakes the parent, `schedule()`s away, and **returns** when
SIGCONT later marks the task `READY` (the saved frame `tf` is untouched, so the iret
resumes the user exactly where it stopped).

Because delivery is always in the **victim's own context** (its return-to-user), the
terminate/stop paths just act on `current()` and `schedule()` away — no cross-stack
surgery. Posting from another context only sets `pending` (+ wakes a blocked target so it
reaches its return-to-user); the action runs later in the target.

### Catchable handlers — the x86 signal frame

`archPushSignalFrame(tf, handler, restorer, sig)` writes onto the **user** stack (mapped
under the current CR3) and rewrites `tf`:

```
user stack (low → high):
  [esp+0]  restorer        <- handler's "return address"
  [esp+4]  sig             <- handler's cdecl arg1
  [esp+8]  SavedContext { eip, eflags, eax, ecx, edx, ebx, useresp, ebp, esi, edi, oldBlocked }
tf.eip = handler;  tf.useresp = esp;   (cs/ss/eflags kept; eflags sanitized on return)
```

The handler runs as `handler(sig)` in ring 3. Its `ret` pops `restorer` and enters the
libc trampoline `__nx_sigtramp`:

```asm
__nx_sigtramp:        ; user space (user/crt or libnanos)
    popl %eax         ; discard sig; esp now points at SavedContext
    movl $119, %eax   ; SYS_sigreturn
    int  $0x80
```

`SYS_sigreturn` reads `SavedContext` at `tf->useresp`, restores all GP regs + eip +
useresp + the saved `blocked` mask into `*tf`, sanitizes eflags (`(saved & SAFE) | 0x202`,
IF forced on, IOPL kept 0), and returns the saved `eax` (so the dispatch's
`r->eax = ...` lands the right value). The iret then resumes the interrupted code.

`signal()` libc wrapper passes `&__nx_sigtramp` as the restorer via SYS_signal's 3rd arg;
the kernel stores it in `SignalState.restorer`.

### Console → signals (the tty front-end)

`KeyDecoder` already tracks Ctrl and emits `letter & 0x1F`. In **cooked** mode
`input_x86.cpp` maps control bytes to a single MI hook `kernel::consoleSignal(sig)`:
`0x03→SIGINT, 0x1C→SIGQUIT, 0x1A→SIGTSTP`. (`0x1C` = Ctrl+\ ; KeyDecoder gains a
Ctrl + `\` case since `\` is not a letter.) `consoleSignal` posts to the foreground
process (the pid the shell is blocked on in waitpid) and wakes it if blocked. In **raw**
mode (the prompt) the bytes are delivered to nsh, which treats `0x03` as cancel-line.

`read()` on a console fd becomes interruptible: after `Scheduler::block()` returns,
`inputRead` checks `kernel::hasPendingSignal()` and returns `-EINTR` (-4) so the read
syscall unwinds to its return-to-user delivery point.

### Job control — `nsh`

`waitpid` gains real `options` (`WNOHANG`, `WUNTRACED`) and the glibc-compatible status
encoding so picolibc's `WIFEXITED/WIFSIGNALED/WIFSTOPPED/WEXITSTATUS/WTERMSIG/WSTOPSIG`
work:
- exit:    `(code & 0xFF) << 8`
- signal:  `sig` (`0 < sig < 0x7F`)
- stopped: `(sig << 8) | 0x7F`

`nsh` keeps a job table `{ pid, state(running/stopped), cmd }`:
- foreground launch → `waitpid(pid, &st, WUNTRACED)`; `WIFSTOPPED` → record job, print
  `\n[n]+ Stopped   <cmd>`, return to prompt.
- `jobs` lists; `fg [n]` → `kill(pid, SIGCONT)` + foreground `waitpid`; `bg [n]` →
  `kill(pid, SIGCONT)`, leave running, don't block.
- each prompt polls `waitpid(-1, &st, WNOHANG|WUNTRACED)` to reap/announce background
  jobs (`[n]+ Done` / `Stopped`).

## Syscalls (Linux i386 numbers)

`SYS_kill 37, SYS_signal 48, SYS_sigreturn 119, SYS_sigprocmask 126`. (`sigaction 67`
optional; `signal()` suffices and carries the restorer.)

## MI / MD split

- MI: `kernel/Signal.{h,cpp}` (pure logic, host-tested), `signalDeliver`/`consoleSignal`/
  `signalSend`/`hasPendingSignal` glue (in `Signal.cpp`/`Exec.cpp`), `procKill`/`procStop`,
  Process fields, `waitpid` status, nsh job control.
- MD (x86): `archPushSignalFrame`, `archSigreturn`, `arch::deliverSignals`, the two
  return-to-user call sites, the `TASK_STOPPED` runnable check is MI (Scheduler).
- `make check-arch` must stay clean (no Registers/port I/O in MI).

## Staged delivery (one commit each; build + boot + `make test` + `check-arch` green)

- **A. Signal core (MI).** `Signal.{h,cpp}` + Process fields + alloc/fork/execve wiring +
  host tests (`test_signal.cpp`). No behavior change yet.
- **B. kill + default-action delivery.** Syscalls (kill/signal/sigprocmask stored),
  `signalDeliver` with TERM/IGN only, `procKill`, the two return-to-user call sites,
  Ctrl+C→SIGINT, `read` EINTR, `waitpid` `WIFSIGNALED` status. QEMU: Ctrl+C kills a
  CPU-bound *and* a read-blocked child; `$?`=130; no faults.
- **C. Catchable handlers.** `archPushSignalFrame`/`archSigreturn`, `__nx_sigtramp`,
  `signal()` wrapper, handler block-mask. QEMU: a program that catches SIGINT runs its
  handler instead of dying.
- **D. Job control.** `TASK_STOPPED` + SIGTSTP/SIGCONT default actions, `procStop`,
  `waitpid` WNOHANG/WUNTRACED + stopped reporting, Ctrl+Z / Ctrl+\, nsh `jobs`/`fg`/`bg`.
  QEMU: Ctrl+Z stops, `jobs` lists, `fg` resumes to completion.

## Testability (host doctest, ≥90% gate)

- `Signal.cpp` pure functions: post/next/resolve/default, SIGCONT↔stop interplay,
  KILL/STOP uncatchable & unblockable, fork-inherit, exec-reset.
- `reapChild` extended: stopped reporting, WNOHANG, status for exited vs stopped.
- Status-encoding helper (`waitStatusFor(exited, code, signal, stopped)`) host-tested
  against the W* macro contract.
- Frame math (`buildSigFrameLayout(useresp, sizeofctx)`) host-tested if extracted.
- Hardware (QEMU only, excluded from the gate): the iret/sigframe push, sigreturn, the
  return-to-user delivery, EINTR wake, TASK_STOPPED scheduling.

## Risks / gotchas

- Deliver **only** when returning to ring 3 (`cs & 3 == 3`); never rewrite a kernel frame.
- `procStop` must be re-entrant-return (resumes on SIGCONT); `procKill` is noreturn.
- Block the delivered signal during its handler; restore the old mask in `sigreturn`
  (else a handler that faults/loops re-enters infinitely).
- Sanitize eflags from the user `SavedContext` (force IF=1, IOPL=0) — a malicious handler
  must not raise IOPL or clear IF.
- SIGKILL/SIGSTOP bypass handler + blocked mask.
- Background child sharing the tty: output interleaves with the prompt (acceptable);
  SIGTTIN/SIGTTOU for bg reads is out of scope.

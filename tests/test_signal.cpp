#include "doctest.h"
#include "Signal.h"

using namespace kernel;

TEST_CASE("sigInit clears everything to defaults") {
	ThreadSignals ts;
	ProcSignals ps;
	sigInit(ts);
	sigInit(ps);
	CHECK(ts.pending == 0);
	CHECK(ts.blocked == 0);
	CHECK(ps.pending == 0);
	CHECK(ps.restart == 0);
	CHECK(ps.restorer == 0);
	for (int i = 0; i < NANOS_NSIG; i++)
		CHECK(ps.handlers[i] == kSigDefault);
	CHECK(sigNextDeliverable(ts, ps) == 0);
}

TEST_CASE("signal mask is per-thread; dispositions are process-wide") {
	kernel::ThreadSignals ts1{}, ts2{};
	kernel::ProcSignals  ps{};
	ps.setHandler(SIGUSR1, (void*)0x1234);
	ts1.block(SIGUSR1);
	CHECK(ts1.isBlocked(SIGUSR1));
	CHECK(!ts2.isBlocked(SIGUSR1));            // independent masks
	CHECK(ps.handler(SIGUSR1) == (void*)0x1234); // shared dispositions
}

TEST_CASE("SIGCONT cancels every pending stop signal, including SIGTTIN/SIGTTOU") {
	ProcSignals ps;
	sigInit(ps);
	sigPost(ps, SIGTTIN);
	sigPost(ps, SIGTTOU);
	sigPost(ps, SIGTSTP);
	CHECK((ps.pending & (1u << (SIGTTIN - 1))) != 0);   // all three pending first
	sigPost(ps, SIGCONT);                               // ... then SIGCONT cancels them all
	CHECK((ps.pending & (1u << (SIGSTOP - 1))) == 0);
	CHECK((ps.pending & (1u << (SIGTSTP - 1))) == 0);
	CHECK((ps.pending & (1u << (SIGTTIN - 1))) == 0);
	CHECK((ps.pending & (1u << (SIGTTOU - 1))) == 0);
	CHECK((ps.pending & (1u << (SIGCONT - 1))) != 0);   // SIGCONT itself stays pending
}

TEST_CASE("sigPost/sigNextDeliverable: lowest pending unblocked signal") {
	ThreadSignals ts;
	ProcSignals ps;
	sigInit(ts);
	sigInit(ps);
	sigPost(ps, SIGTERM);
	sigPost(ps, SIGINT);
	CHECK(sigNextDeliverable(ts, ps) == SIGINT);   // 2 < 15
	sigConsume(ts, ps, SIGINT);
	CHECK(sigNextDeliverable(ts, ps) == SIGTERM);
	sigConsume(ts, ps, SIGTERM);
	CHECK(sigNextDeliverable(ts, ps) == 0);
}

TEST_CASE("thread-directed and process-directed pending both deliver") {
	ThreadSignals ts;
	ProcSignals ps;
	sigInit(ts);
	sigInit(ps);
	sigPost(ts, SIGTERM);                      // thread-directed (e.g. future tgkill)
	sigPost(ps, SIGINT);                       // process-directed (kill)
	CHECK(sigNextDeliverable(ts, ps) == SIGINT);   // lowest of the union
	sigConsume(ts, ps, SIGINT);
	CHECK(sigNextDeliverable(ts, ps) == SIGTERM);
	sigConsume(ts, ps, SIGTERM);               // clears from whichever set held it
	CHECK(sigNextDeliverable(ts, ps) == 0);
	CHECK(ts.pending == 0);
	CHECK(ps.pending == 0);
}

TEST_CASE("blocked signals are not deliverable, except SIGKILL/SIGSTOP") {
	ThreadSignals ts;
	ProcSignals ps;
	sigInit(ts);
	sigInit(ps);
	ts.blocked = ~0u;                          // block everything
	sigPost(ps, SIGINT);
	CHECK(sigNextDeliverable(ts, ps) == 0);    // SIGINT is blocked

	sigPost(ps, SIGKILL);
	CHECK(sigNextDeliverable(ts, ps) == SIGKILL);  // unblockable
	sigConsume(ts, ps, SIGKILL);
	sigPost(ps, SIGSTOP);
	CHECK(sigNextDeliverable(ts, ps) == SIGSTOP);  // unblockable
}

TEST_CASE("sigDefaultAction classifies the common signals") {
	CHECK(sigDefaultAction(SIGINT) == SD_TERM);
	CHECK(sigDefaultAction(SIGTERM) == SD_TERM);
	CHECK(sigDefaultAction(SIGKILL) == SD_TERM);
	CHECK(sigDefaultAction(SIGQUIT) == SD_CORE);
	CHECK(sigDefaultAction(SIGCHLD) == SD_IGN);
	CHECK(sigDefaultAction(SIGCONT) == SD_CONT);
	CHECK(sigDefaultAction(SIGSTOP) == SD_STOP);
	CHECK(sigDefaultAction(SIGTSTP) == SD_STOP);
}

TEST_CASE("sigResolve folds the handler table and the default action") {
	ProcSignals ps;
	sigInit(ps);
	CHECK(sigResolve(ps, SIGINT) == DISP_TERM);    // DFL -> terminate
	CHECK(sigResolve(ps, SIGCHLD) == DISP_IGN);    // DFL -> ignore
	CHECK(sigResolve(ps, SIGTSTP) == DISP_STOP);   // DFL -> stop
	CHECK(sigResolve(ps, SIGCONT) == DISP_CONT);

	ps.handlers[SIGINT] = kSigIgnore;
	CHECK(sigResolve(ps, SIGINT) == DISP_IGN);
	ps.handlers[SIGINT] = 0x401000;                // a user handler address
	CHECK(sigResolve(ps, SIGINT) == DISP_HANDLER);

	// SIGKILL/SIGSTOP ignore any installed handler.
	ps.handlers[SIGKILL] = 0x401000;
	ps.handlers[SIGSTOP] = 0x401000;
	CHECK(sigResolve(ps, SIGKILL) == DISP_TERM);
	CHECK(sigResolve(ps, SIGSTOP) == DISP_STOP);
	CHECK(!sigCanCatch(SIGKILL));
	CHECK(!sigCanCatch(SIGSTOP));
	CHECK(sigCanCatch(SIGINT));
}

TEST_CASE("SIGCONT and stop signals cancel each other when posted") {
	ThreadSignals ts;
	ProcSignals ps;
	sigInit(ts);
	sigInit(ps);
	sigPost(ps, SIGTSTP);
	sigPost(ps, SIGCONT);                          // cancels the pending stop
	CHECK(sigNextDeliverable(ts, ps) == SIGCONT);

	sigInit(ps);
	sigPost(ps, SIGCONT);
	sigPost(ps, SIGTSTP);                          // cancels the pending cont
	CHECK(sigNextDeliverable(ts, ps) == SIGTSTP);
}

TEST_CASE("sigForkInherit copies dispositions + mask, drops pending") {
	ProcSignals pps;
	ThreadSignals pts;
	sigInit(pps);
	sigInit(pts);
	pps.handlers[SIGINT] = 0x401000;
	pps.handlers[SIGQUIT] = kSigIgnore;
	pts.blocked = 0x8;
	pps.restart = 0x2;
	pps.restorer = 0x402000;
	sigPost(pps, SIGTERM);

	ProcSignals cps;
	ThreadSignals cts;
	sigForkInherit(cps, pps);                      // dispositions (process-wide)
	sigForkInherit(cts, pts);                      // mask (per-thread)
	CHECK(cps.handlers[SIGINT] == 0x401000u);
	CHECK(cps.handlers[SIGQUIT] == kSigIgnore);
	CHECK(cts.blocked == 0x8u);
	CHECK(cps.restart == 0x2u);                    // SA_RESTART flags inherited
	CHECK(cps.restorer == 0x402000u);
	CHECK(cps.pending == 0);                        // pending NOT inherited
	CHECK(cts.pending == 0);
}

TEST_CASE("sigExecReset: caught -> DFL, ignored stays ignored, pending dropped") {
	ProcSignals ps;
	ThreadSignals ts;
	sigInit(ps);
	sigInit(ts);
	ps.handlers[SIGINT] = 0x401000;                // caught
	ps.handlers[SIGQUIT] = kSigIgnore;              // ignored
	ts.blocked = 0x4;
	ps.restart = 0x6;
	sigPost(ps, SIGTERM);

	sigExecReset(ts, ps);
	CHECK(ps.handlers[SIGINT] == kSigDefault);       // reset
	CHECK(ps.handlers[SIGQUIT] == kSigIgnore);      // preserved
	CHECK(ts.blocked == 0x4u);                     // mask preserved
	CHECK(ps.restart == 0);                        // restart flags cleared (handlers gone)
	CHECK(ps.pending == 0);
	CHECK(ts.pending == 0);
}

TEST_CASE("sigHasInterrupt: ignored/cont signals do not interrupt; term/stop/handler do") {
	ThreadSignals ts;
	ProcSignals ps;
	sigInit(ts);
	sigInit(ps);
	CHECK(!sigHasInterrupt(ts, ps));

	sigPost(ps, SIGCHLD);                     // default-ignore -> no interrupt (no EINTR)
	CHECK(!sigHasInterrupt(ts, ps));

	sigPost(ps, SIGINT);                      // default-terminate -> interrupts
	CHECK(sigHasInterrupt(ts, ps));

	sigInit(ps);
	sigPost(ps, SIGCONT);                     // resume -> does not interrupt a wait
	CHECK(!sigHasInterrupt(ts, ps));

	sigInit(ps);
	sigPost(ps, SIGTSTP);                     // stop -> interrupts
	CHECK(sigHasInterrupt(ts, ps));

	sigInit(ps);
	ps.handlers[SIGINT] = kSigIgnore;         // explicitly ignored -> no interrupt
	sigPost(ps, SIGINT);
	CHECK(!sigHasInterrupt(ts, ps));

	ps.handlers[SIGINT] = 0x401000;           // a handler -> interrupts
	CHECK(sigHasInterrupt(ts, ps));

	sigInit(ps);                              // blocked terminate -> not deliverable, no interrupt
	ts.blocked = ~0u;
	sigPost(ps, SIGINT);
	CHECK(!sigHasInterrupt(ts, ps));
}

TEST_CASE("wait-status encodings match the W* macro contract") {
	// WIFEXITED: (status & 0x7f) == 0; WEXITSTATUS: (status >> 8) & 0xff
	int e = waitStatusExited(42);
	CHECK((e & 0x7f) == 0);
	CHECK(((e >> 8) & 0xff) == 42);

	// WIFSIGNALED: (status & 0x7f) is the signal, neither 0 nor 0x7f
	int g = waitStatusSignalled(SIGINT);
	CHECK((g & 0x7f) == SIGINT);
	CHECK((g & 0x7f) != 0);
	CHECK((g & 0x7f) != 0x7f);

	// WIFSTOPPED: (status & 0xff) == 0x7f; WSTOPSIG: (status >> 8) & 0xff
	int st = waitStatusStopped(SIGTSTP);
	CHECK((st & 0xff) == 0x7f);
	CHECK(((st >> 8) & 0xff) == SIGTSTP);
}

TEST_CASE("64-bit masks: real-time signals 32..64 are independent of 1..31") {
	// The disposition table now spans the full range, and SIGCANCEL=32 is reserved.
	CHECK(NANOS_NSIG == 65);
	CHECK(SIGCANCEL == 32);

	ThreadSignals ts{};
	sigInit(ts);

	// Signal 31 (highest of the legacy 32-bit word) and the new real-time signals.
	ts.block(31);
	ts.block(SIGCANCEL);   // 32 — the lowest bit of the high word
	ts.block(64);          // 64 — the highest valid signal
	CHECK(ts.isBlocked(31));
	CHECK(ts.isBlocked(SIGCANCEL));
	CHECK(ts.isBlocked(64));
	// The masks really use the high 32 bits.
	CHECK((ts.blocked & (1ull << 30)) != 0);   // bit (31-1)
	CHECK((ts.blocked & (1ull << 31)) != 0);   // bit (32-1) — SIGCANCEL
	CHECK((ts.blocked & (1ull << 63)) != 0);   // bit (64-1)

	// Clearing a high-word signal leaves the low word untouched (independent bits).
	ts.unblock(SIGCANCEL);
	CHECK(!ts.isBlocked(SIGCANCEL));
	CHECK(ts.isBlocked(31));
	CHECK(ts.isBlocked(64));

	// 65 is out of range and must not be accepted.
	ts.block(65);
	CHECK(!ts.isBlocked(65));

	// Pending tracking works across the full 64-bit width too.
	ProcSignals ps{};
	sigInit(ps);
	sigPost(ps, 64);
	CHECK((ps.pending & (1ull << 63)) != 0);
}

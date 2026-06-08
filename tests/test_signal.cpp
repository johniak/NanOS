#include "doctest.h"
#include "Signal.h"

using namespace kernel;

TEST_CASE("sigInit clears everything to defaults") {
	SignalState s;
	sigInit(s);
	CHECK(s.pending == 0);
	CHECK(s.blocked == 0);
	CHECK(s.restorer == 0);
	for (int i = 0; i < NANOS_NSIG; i++)
		CHECK(s.handlers[i] == kSigDefault);
	CHECK(sigNextDeliverable(s) == 0);
}

TEST_CASE("sigPost/sigNextDeliverable: lowest pending unblocked signal") {
	SignalState s;
	sigInit(s);
	sigPost(s, SIGTERM);
	sigPost(s, SIGINT);
	CHECK(sigNextDeliverable(s) == SIGINT);   // 2 < 15
	sigConsume(s, SIGINT);
	CHECK(sigNextDeliverable(s) == SIGTERM);
	sigConsume(s, SIGTERM);
	CHECK(sigNextDeliverable(s) == 0);
}

TEST_CASE("blocked signals are not deliverable, except SIGKILL/SIGSTOP") {
	SignalState s;
	sigInit(s);
	s.blocked = ~0u;                          // block everything
	sigPost(s, SIGINT);
	CHECK(sigNextDeliverable(s) == 0);        // SIGINT is blocked

	sigPost(s, SIGKILL);
	CHECK(sigNextDeliverable(s) == SIGKILL);  // unblockable
	sigConsume(s, SIGKILL);
	sigPost(s, SIGSTOP);
	CHECK(sigNextDeliverable(s) == SIGSTOP);  // unblockable
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
	SignalState s;
	sigInit(s);
	CHECK(sigResolve(s, SIGINT) == DISP_TERM);    // DFL -> terminate
	CHECK(sigResolve(s, SIGCHLD) == DISP_IGN);    // DFL -> ignore
	CHECK(sigResolve(s, SIGTSTP) == DISP_STOP);   // DFL -> stop
	CHECK(sigResolve(s, SIGCONT) == DISP_CONT);

	s.handlers[SIGINT] = kSigIgnore;
	CHECK(sigResolve(s, SIGINT) == DISP_IGN);
	s.handlers[SIGINT] = 0x401000;                // a user handler address
	CHECK(sigResolve(s, SIGINT) == DISP_HANDLER);

	// SIGKILL/SIGSTOP ignore any installed handler.
	s.handlers[SIGKILL] = 0x401000;
	s.handlers[SIGSTOP] = 0x401000;
	CHECK(sigResolve(s, SIGKILL) == DISP_TERM);
	CHECK(sigResolve(s, SIGSTOP) == DISP_STOP);
	CHECK(!sigCanCatch(SIGKILL));
	CHECK(!sigCanCatch(SIGSTOP));
	CHECK(sigCanCatch(SIGINT));
}

TEST_CASE("SIGCONT and stop signals cancel each other when posted") {
	SignalState s;
	sigInit(s);
	sigPost(s, SIGTSTP);
	sigPost(s, SIGCONT);                          // cancels the pending stop
	CHECK(sigNextDeliverable(s) == SIGCONT);

	sigInit(s);
	sigPost(s, SIGCONT);
	sigPost(s, SIGTSTP);                          // cancels the pending cont
	CHECK(sigNextDeliverable(s) == SIGTSTP);
}

TEST_CASE("sigForkInherit copies dispositions + mask, drops pending") {
	SignalState parent;
	sigInit(parent);
	parent.handlers[SIGINT] = 0x401000;
	parent.handlers[SIGQUIT] = kSigIgnore;
	parent.blocked = 0x8;
	parent.restorer = 0x402000;
	sigPost(parent, SIGTERM);

	SignalState child;
	sigForkInherit(child, parent);
	CHECK(child.handlers[SIGINT] == 0x401000u);
	CHECK(child.handlers[SIGQUIT] == kSigIgnore);
	CHECK(child.blocked == 0x8u);
	CHECK(child.restorer == 0x402000u);
	CHECK(child.pending == 0);                    // pending NOT inherited
}

TEST_CASE("sigExecReset: caught -> DFL, ignored stays ignored, pending dropped") {
	SignalState s;
	sigInit(s);
	s.handlers[SIGINT] = 0x401000;                // caught
	s.handlers[SIGQUIT] = kSigIgnore;                // ignored
	s.blocked = 0x4;
	sigPost(s, SIGTERM);

	sigExecReset(s);
	CHECK(s.handlers[SIGINT] == kSigDefault);         // reset
	CHECK(s.handlers[SIGQUIT] == kSigIgnore);        // preserved
	CHECK(s.blocked == 0x4u);                     // mask preserved
	CHECK(s.pending == 0);
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

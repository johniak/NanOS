#include "Signal.h"

namespace kernel {

static unsigned bit(int sig) { return 1u << (sig - 1); }
static bool valid(int sig) { return sig > 0 && sig < NANOS_NSIG; }

SigDefault sigDefaultAction(int sig) {
	switch (sig) {
	case SIGCHLD:
		return SD_IGN;                 // ignored by default
	case SIGCONT:
		return SD_CONT;                // resume a stopped process
	case SIGSTOP:
	case SIGTSTP:
		return SD_STOP;                // stop (job control)
	case SIGQUIT:
	case SIGILL:
	case SIGABRT:
	case SIGFPE:
	case SIGSEGV:
	case SIGTRAP:
		return SD_CORE;                // terminate (we have no core dump, treat as TERM)
	default:
		return SD_TERM;                // SIGHUP/SIGINT/SIGKILL/SIGPIPE/SIGALRM/SIGTERM/...
	}
}

bool sigCanCatch(int sig) {
	return sig != SIGKILL && sig != SIGSTOP;
}

void sigInit(SignalState& s) {
	s.pending = 0;
	s.blocked = 0;
	s.restorer = 0;
	for (int i = 0; i < NANOS_NSIG; i++)
		s.handlers[i] = kSigDefault;
}

// Posting a signal sets its pending bit. SIGCONT and the stop signals are mutually
// exclusive: a pending SIGCONT cancels pending stops and vice versa (Linux semantics).
void sigPost(SignalState& s, int sig) {
	if (!valid(sig))
		return;
	if (sig == SIGCONT)
		s.pending &= ~(bit(SIGSTOP) | bit(SIGTSTP));
	else if (sigDefaultAction(sig) == SD_STOP)
		s.pending &= ~bit(SIGCONT);
	s.pending |= bit(sig);
}

void sigConsume(SignalState& s, int sig) {
	if (valid(sig))
		s.pending &= ~bit(sig);
}

// Lowest-numbered deliverable signal. SIGKILL/SIGSTOP cannot be blocked.
int sigNextDeliverable(const SignalState& s) {
	unsigned ready = s.pending & ~s.blocked;
	ready |= s.pending & (bit(SIGKILL) | bit(SIGSTOP));   // these ignore the mask
	for (int sig = 1; sig < NANOS_NSIG; sig++)
		if (ready & bit(sig))
			return sig;
	return 0;
}

SigDisp sigResolve(const SignalState& s, int sig) {
	// SIGKILL/SIGSTOP are immune to handlers.
	if (sig == SIGKILL)
		return DISP_TERM;
	if (sig == SIGSTOP)
		return DISP_STOP;

	unsigned h = valid(sig) ? s.handlers[sig] : kSigDefault;
	if (h == kSigIgnore)
		return DISP_IGN;
	if (h != kSigDefault)
		return DISP_HANDLER;

	switch (sigDefaultAction(sig)) {
	case SD_IGN:  return DISP_IGN;
	case SD_STOP: return DISP_STOP;
	case SD_CONT: return DISP_CONT;
	default:      return DISP_TERM;   // SD_TERM / SD_CORE
	}
}

void sigForkInherit(SignalState& child, const SignalState& parent) {
	child.blocked = parent.blocked;
	child.restorer = parent.restorer;
	child.pending = 0;                        // pending signals are NOT inherited
	for (int i = 0; i < NANOS_NSIG; i++)
		child.handlers[i] = parent.handlers[i];
}

// execve keeps ignored signals ignored but resets caught ones to the default; pending
// signals are dropped. The blocked mask is preserved (Linux preserves it across exec).
void sigExecReset(SignalState& s) {
	s.pending = 0;
	for (int i = 0; i < NANOS_NSIG; i++)
		if (s.handlers[i] != kSigIgnore)
			s.handlers[i] = kSigDefault;
}

int waitStatusExited(int code)    { return (code & 0xFF) << 8; }
int waitStatusSignalled(int sig)  { return sig & 0x7F; }
int waitStatusStopped(int sig)    { return ((sig & 0xFF) << 8) | 0x7F; }

}  // namespace kernel

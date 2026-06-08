/*
 * Signal.h — the machine-independent signal core.
 *
 * Per-process signal state (pending/blocked masks + a disposition table) plus the pure
 * policy functions that decide what a given signal does. No kernel dependencies, so the
 * whole decision logic is host-testable. Delivery (building the user-stack frame, killing
 * or stopping a task) is the caller's job — see kernel/Exec.cpp + arch/usermode.
 */
#ifndef SIGNAL_H_
#define SIGNAL_H_

// Signal numbers (Linux i386 ABI). Defined as guarded macros so they coexist with the
// host libc's <signal.h> (doctest pulls it in; the values match on our Linux test host),
// while the freestanding kernel build — which has no <signal.h> — gets them from here.
#ifndef SIGHUP
#define SIGHUP 1
#endif
#ifndef SIGINT
#define SIGINT 2
#endif
#ifndef SIGQUIT
#define SIGQUIT 3
#endif
#ifndef SIGILL
#define SIGILL 4
#endif
#ifndef SIGTRAP
#define SIGTRAP 5
#endif
#ifndef SIGABRT
#define SIGABRT 6
#endif
#ifndef SIGFPE
#define SIGFPE 8
#endif
#ifndef SIGKILL
#define SIGKILL 9
#endif
#ifndef SIGSEGV
#define SIGSEGV 11
#endif
#ifndef SIGPIPE
#define SIGPIPE 13
#endif
#ifndef SIGALRM
#define SIGALRM 14
#endif
#ifndef SIGTERM
#define SIGTERM 15
#endif
#ifndef SIGCHLD
#define SIGCHLD 17
#endif
#ifndef SIGCONT
#define SIGCONT 18
#endif
#ifndef SIGSTOP
#define SIGSTOP 19
#endif
#ifndef SIGTSTP
#define SIGTSTP 20
#endif

#define NANOS_NSIG 32   // disposition-table size; valid signals are 1..NANOS_NSIG-1

namespace kernel {

// Disposition values stored in SignalState::handlers[]. NOT the libc SIG_DFL/SIG_IGN
// pointer-cast macros — these are plain small integers compared against the unsigned
// handler slots (a real handler is its >1 user-space address).
static const unsigned kSigDefault = 0;   // SIG_DFL
static const unsigned kSigIgnore  = 1;   // SIG_IGN

struct SignalState {
	unsigned pending;                  // bit (sig-1) set => sig is pending
	unsigned blocked;                  // sigprocmask: blocked signals
	unsigned handlers[NANOS_NSIG];     // kSigDefault / kSigIgnore / user handler address
	unsigned restorer;                 // sa_restorer trampoline (libc __nx_sigtramp)
};

// Default action of a signal when its disposition is the default.
enum SigDefault { SD_TERM, SD_IGN, SD_CORE, SD_STOP, SD_CONT };
SigDefault sigDefaultAction(int sig);

// What delivery should actually do with a pending signal, after folding the handler
// table and the default action (SIGKILL always TERM, SIGSTOP always STOP).
enum SigDisp { DISP_TERM, DISP_IGN, DISP_STOP, DISP_CONT, DISP_HANDLER };

void sigInit(SignalState& s);
void sigPost(SignalState& s, int sig);          // set pending (+ stop/cont interplay)
void sigConsume(SignalState& s, int sig);        // clear pending bit
int  sigNextDeliverable(const SignalState& s);   // lowest pending & ~blocked sig, or 0
SigDisp sigResolve(const SignalState& s, int sig);
bool sigCanCatch(int sig);                       // false for SIGKILL/SIGSTOP

void sigForkInherit(SignalState& child, const SignalState& parent);  // copy disp+mask
void sigExecReset(SignalState& s);               // caught -> default (keep ignore)

// Encode a child status the glibc/picolibc W* macros understand:
//   exited  -> (code & 0xFF) << 8     (WIFEXITED)
//   signal  -> sig                    (WIFSIGNALED, 0 < sig < 0x7F)
//   stopped -> (sig << 8) | 0x7F      (WIFSTOPPED)
int waitStatusExited(int code);
int waitStatusSignalled(int sig);
int waitStatusStopped(int sig);

}  // namespace kernel

#endif /* SIGNAL_H_ */

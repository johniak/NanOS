/*
 * Signal.h — the machine-independent signal core.
 *
 * Signal state is split the POSIX way: the parts that are PER-THREAD (the pending +
 * blocked masks — `ThreadSignals`) versus the parts SHARED by every thread of a process
 * (the disposition table + SA_RESTART/restorer + a process-directed pending set —
 * `ProcSignals`). With one thread per process the two together behave exactly like the
 * old monolithic state. The pure policy functions that decide what a given signal does
 * take the relevant halves. No kernel dependencies, so the whole decision logic is
 * host-testable. Delivery (building the user-stack frame, killing or stopping a task) is
 * the caller's job — see kernel/Exec.cpp + arch/usermode.
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
#ifndef SIGTTIN
#define SIGTTIN 21
#endif
#ifndef SIGTTOU
#define SIGTTOU 22
#endif
#ifndef SIGUSR1
#define SIGUSR1 10
#endif
#ifndef SIGUSR2
#define SIGUSR2 12
#endif

#define NANOS_NSIG 32   // disposition-table size; valid signals are 1..NANOS_NSIG-1

namespace kernel {

// Disposition values stored in ProcSignals::handlers[]. NOT the libc SIG_DFL/SIG_IGN
// pointer-cast macros — these are plain small integers compared against the unsigned
// handler slots (a real handler is its >1 user-space address).
static const unsigned kSigDefault = 0;   // SIG_DFL
static const unsigned kSigIgnore  = 1;   // SIG_IGN

// PER-THREAD signal state: each thread has its own pending set and its own block mask
// (sigprocmask is per-thread in POSIX). Lives on Thread::sig.
struct ThreadSignals {
	unsigned pending;                  // bit (sig-1) set => sig is pending FOR THIS THREAD
	                                   // (thread-directed; populated by tgkill in Task 5.2)
	unsigned blocked;                  // sigprocmask: this thread's blocked signals

	void block(int sig);               // add sig to the block mask
	void unblock(int sig);             // remove sig from the block mask
	bool isBlocked(int sig) const;     // is sig currently blocked?
};

// PER-PROCESS signal state, shared by every thread of the thread group: the disposition
// table, the SA_RESTART set + restorer trampoline, and a process-directed pending set (a
// signal sent to the process — kill(pid) — lands here until a thread picks it up). Lives
// on Process::psig.
struct ProcSignals {
	unsigned pending;                  // process-directed pending (kill(pid)); any thread may take it
	unsigned restart;                  // bit set => this signal's handler has SA_RESTART
	unsigned handlers[NANOS_NSIG];     // kSigDefault / kSigIgnore / user handler address
	unsigned restorer;                 // sa_restorer trampoline (libc __nx_sigtramp)

	void  setHandler(int sig, void* h);  // install a disposition (DFL/IGN/handler address)
	void* handler(int sig) const;        // read the current disposition as a pointer
};

// Default action of a signal when its disposition is the default.
enum SigDefault { SD_TERM, SD_IGN, SD_CORE, SD_STOP, SD_CONT };
SigDefault sigDefaultAction(int sig);

// What delivery should actually do with a pending signal, after folding the handler
// table and the default action (SIGKILL always TERM, SIGSTOP always STOP).
enum SigDisp { DISP_TERM, DISP_IGN, DISP_STOP, DISP_CONT, DISP_HANDLER };

void sigInit(ThreadSignals& s);                  // pending=0, blocked=0
void sigInit(ProcSignals& s);                    // pending=0, restart=0, restorer=0, all DFL
void sigPost(ThreadSignals& s, int sig);         // set thread-directed pending (+ stop/cont interplay)
void sigPost(ProcSignals& s, int sig);           // set process-directed pending (+ stop/cont interplay)
void sigConsume(ThreadSignals& ts, ProcSignals& ps, int sig);   // clear the pending bit in both sets
// Lowest-numbered deliverable signal across the thread's pending and the process-directed
// pending, masked by this thread's blocked set (SIGKILL/SIGSTOP ignore the mask). 0 if none.
int  sigNextDeliverable(const ThreadSignals& ts, const ProcSignals& ps);
SigDisp sigResolve(const ProcSignals& ps, int sig);   // fold the (process-wide) disposition table
bool sigCanCatch(int sig);                       // false for SIGKILL/SIGSTOP

// True if a deliverable signal (thread-directed or process-directed) would actually
// interrupt the thread — i.e. resolves to terminate / stop / run-a-handler. Ignored
// signals (e.g. default SIGCHLD) and SIGCONT do NOT count, so they must not yield EINTR
// from a blocking syscall.
bool sigHasInterrupt(const ThreadSignals& ts, const ProcSignals& ps);

void sigForkInherit(ProcSignals& child, const ProcSignals& parent);    // copy dispositions, drop pending
void sigForkInherit(ThreadSignals& child, const ThreadSignals& parent);// copy mask, drop pending
// execve: caught dispositions -> default (ignored stay ignored), SA_RESTART flags cleared,
// and all pending dropped (both the process-directed set and this thread's). The block mask
// is preserved (Linux preserves it across exec).
void sigExecReset(ThreadSignals& ts, ProcSignals& ps);   // arg order matches the rest of the split API: (thread, proc)

// Encode a child status the glibc/picolibc W* macros understand:
//   exited  -> (code & 0xFF) << 8     (WIFEXITED)
//   signal  -> sig                    (WIFSIGNALED, 0 < sig < 0x7F)
//   stopped -> (sig << 8) | 0x7F      (WIFSTOPPED)
int waitStatusExited(int code);
int waitStatusSignalled(int sig);
int waitStatusStopped(int sig);

}  // namespace kernel

#endif /* SIGNAL_H_ */

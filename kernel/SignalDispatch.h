/*
 * SignalDispatch.h — kernel-glue signal entry points (declarations only).
 *
 * The pure policy lives in Signal.{h,cpp}; these functions wire it to the process table,
 * the scheduler and the arch return-to-user path. Defined in kernel/Exec.cpp. Kept in a
 * lean header (just a forward-declared TrapFrame) so the MD trap/IRQ/input files can call
 * them without pulling in the VFS.
 */
#pragma once

#include <stdint.h>   // uint64_t — the rt_* glue carries the 64-bit signal mask

namespace arch { struct TrapFrame; }

// The kernel-ABI sigaction struct rt_sigaction reads/writes (defined in SyscallNr.h). Only
// a forward declaration is needed here; the dispatch includes SyscallNr.h for the layout.
struct k_sigaction;

namespace kernel {

// A blocking syscall interrupted by a signal returns this internal sentinel (Linux's
// ERESTARTSYS). It never reaches user space: signal delivery either restarts the syscall
// (SA_RESTART) or rewrites it to -EINTR.
static const int ERESTARTSYS = 512;

int  signalSend(int pid, int sig);                                  // kill(2)
// Thread-directed delivery (tgkill(2)/tkill(2)): post `sig` to the SPECIFIC thread `tid` and
// wake it. Unlike kill(2) this is the legitimate in-process transport for SIGCANCEL
// (pthread_cancel, Task 5.3), so SIGCANCEL is allowed here. tgid >= 0 also requires the thread
// to belong to that thread group (-ESRCH otherwise); tgid < 0 is the tkill form (no group check).
int  signalSendThread(int tgid, int tid, int sig);                  // tgkill(2) / tkill(2)
int  signalAction(int sig, unsigned handler, unsigned restorer);    // signal(2)
int  signalMask(int how, unsigned set, unsigned* oldset);           // sigprocmask(2) [legacy, low 32]
int  signalPause();                                                 // pause(2)
int  signalSuspend(unsigned mask);                                  // sigsuspend(2)  [legacy, low 32]
// Real-time signal glue (the rt_* syscalls): the mask is carried by pointer so signals
// 32..64 are addressable. sigsetsize MUST be 8 (a 64-bit mask) or these return -EINVAL.
int  signalMaskRt(int how, const uint64_t* set, uint64_t* oldset, unsigned sigsetsize);     // rt_sigprocmask(2)
int  signalSuspendRt(const uint64_t* mask, unsigned sigsetsize);                            // rt_sigsuspend(2)
int  signalActionRt(int sig, const ::k_sigaction* act, ::k_sigaction* old, unsigned sigsetsize); // rt_sigaction(2)
int  signalPendingRt(uint64_t* set, unsigned sigsetsize);                                   // rt_sigpending(2)
// Deliver pending signals at a return to ring 3. `origEax` is the syscall number when
// coming from the syscall path (`inSyscall` true) so an interrupted, restartable syscall
// can be restarted; on the IRQ path pass (0, false).
void signalDeliver(arch::TrapFrame* tf, unsigned origEax, bool inSyscall);
int  signalReturn(arch::TrapFrame* tf);    // sigreturn(2): restore the pre-handler frame
void consoleSignal(int sig);               // a cooked-tty control key -> foreground proc
void consoleSignalGroup(int sig, int pgrp);// tty control key -> foreground process GROUP
int  signalSendGroup(int pgid, int sig);   // post `sig` to every member of a group
// The console's foreground process group (job-control singleton for the physical terminal):
// set by tcsetpgrp (SYS_ioctl TIOCSPGRP on a console fd), read by tcgetpgrp + the SIGTTIN
// gate. 0 = no group has claimed the terminal.
void consoleSetPgrp(int pgrp);
int  consoleGetPgrp();
// Process-group / session syscalls (thin glue over ProcTable bookkeeping).
int  sysSetpgid(int pid, int pgid);
int  sysGetpgid(int pid);
int  sysSetsid();
int  sysGetsid(int pid);
bool hasPendingSignalCurrent();            // EINTR/restart check for blocking syscalls

}  // namespace kernel

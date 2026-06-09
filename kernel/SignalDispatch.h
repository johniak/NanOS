/*
 * SignalDispatch.h — kernel-glue signal entry points (declarations only).
 *
 * The pure policy lives in Signal.{h,cpp}; these functions wire it to the process table,
 * the scheduler and the arch return-to-user path. Defined in kernel/Exec.cpp. Kept in a
 * lean header (just a forward-declared TrapFrame) so the MD trap/IRQ/input files can call
 * them without pulling in the VFS.
 */
#pragma once

namespace arch { struct TrapFrame; }

namespace kernel {

// A blocking syscall interrupted by a signal returns this internal sentinel (Linux's
// ERESTARTSYS). It never reaches user space: signal delivery either restarts the syscall
// (SA_RESTART) or rewrites it to -EINTR.
static const int ERESTARTSYS = 512;

int  signalSend(int pid, int sig);                                  // kill(2)
int  signalAction(int sig, unsigned handler, unsigned restorer);    // signal(2)
int  signalMask(int how, unsigned set, unsigned* oldset);           // sigprocmask(2)
// Deliver pending signals at a return to ring 3. `origEax` is the syscall number when
// coming from the syscall path (`inSyscall` true) so an interrupted, restartable syscall
// can be restarted; on the IRQ path pass (0, false).
void signalDeliver(arch::TrapFrame* tf, unsigned origEax, bool inSyscall);
int  signalReturn(arch::TrapFrame* tf);    // sigreturn(2): restore the pre-handler frame
void consoleSignal(int sig);               // a cooked-tty control key -> foreground proc
void consoleSignalGroup(int sig, int pgrp);// tty control key -> foreground process GROUP
int  signalSendGroup(int pgid, int sig);   // post `sig` to every member of a group
// Process-group / session syscalls (thin glue over ProcTable bookkeeping).
int  sysSetpgid(int pid, int pgid);
int  sysGetpgid(int pid);
int  sysSetsid();
int  sysGetsid(int pid);
bool hasPendingSignalCurrent();            // EINTR/restart check for blocking syscalls

}  // namespace kernel

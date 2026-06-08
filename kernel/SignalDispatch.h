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

int  signalSend(int pid, int sig);                                  // kill(2)
int  signalAction(int sig, unsigned handler, unsigned restorer);    // signal(2)
int  signalMask(int how, unsigned set, unsigned* oldset);           // sigprocmask(2)
void signalDeliver(arch::TrapFrame* tf);   // deliver pending signals at return-to-user
int  signalReturn(arch::TrapFrame* tf);    // sigreturn(2): restore the pre-handler frame
void consoleSignal(int sig);               // a cooked-tty control key -> foreground proc
bool hasPendingSignalCurrent();            // for EINTR in interruptible blocking syscalls

}  // namespace kernel

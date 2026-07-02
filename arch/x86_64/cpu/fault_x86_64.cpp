/*
 * fault_x86_64.cpp — x86-64 CPU-exception handlers (#GP vector 13, #PF vector 14).
 *
 * A fault from ring 3 (cs low 2 bits set) is the user program's bug, NOT the kernel's: kill just
 * that process (like a SIGSEGV) and let the scheduler keep running — a crashing app (e.g. a browser
 * faulting on teardown) must not take the whole system down. A fault from ring 0 is a real kernel
 * bug: print the 64-bit rip/cr2 + halt, so it is debuggable instead of triple-faulting invisibly.
 * (Earlier this stub halted on EVERY fault, including ring 3, because the scheduler wasn't ported
 * yet — now it is, so ring-3 faults route to killCurrentProcess like the i686 handler.)
 */
#include "Interrupt64.h"
#include "PagingControl.h"   // kernel::readCr2 (64-bit)
#include "Console.h"
#include "Exec.h"            // kernel::killCurrentProcess
#include "Process.h"         // kernel::Process::current() (faulting-process name)
#include "Signal.h"          // SIGSEGV
#include "vt/VtManager.h"    // force the text console visible so a kernel panic is on screen

namespace {

void faultHandler(kernel::Registers* r) {
    if (r->cs & 3) {
        kernel::Console::write("\n[nanos: killed faulting process: vec=");
        kernel::Console::writeHex((int) r->int_no);
        if (r->int_no == 14) { kernel::Console::write(" cr2="); kernel::Console::writeHex((unsigned long) kernel::readCr2()); }
        kernel::Console::write(" rip=");
        kernel::Console::writeHex((unsigned long) r->rip);
        if (kernel::Process* cp = kernel::ProcTable::current()) {
            kernel::Console::write(" proc="); kernel::Console::write(cp->comm);
        }
        kernel::Console::writeLine("]");
        kernel::killCurrentProcess(SIGSEGV);   // terminates current + reschedules; does NOT return
        return;                                // (unreachable)
    }
    if (kernel::g_vtmgr) kernel::g_vtmgr->panicSwitchToText();   // make the panic visible over any graphics VT
    kernel::Console::write("\n*** KERNEL EXCEPTION vec=");
    kernel::Console::writeHex((int) r->int_no);
    kernel::Console::write(" err=");
    kernel::Console::writeHex((unsigned long) r->err_code);
    kernel::Console::write(" rip=");
    kernel::Console::writeHex((unsigned long) r->rip);
    kernel::Console::write(" cs=");
    kernel::Console::writeHex((unsigned long) r->cs);
    kernel::Console::write(" rflags=");
    kernel::Console::writeHex((unsigned long) r->rflags);
    if (r->int_no == 14) {        // #PF: CR2 holds the faulting linear address
        kernel::Console::write(" cr2=");
        kernel::Console::writeHex((unsigned long) kernel::readCr2());
    }
    kernel::Console::write("\n    rax=");
    kernel::Console::writeHex((unsigned long) r->rax);
    kernel::Console::write(" rbx=");
    kernel::Console::writeHex((unsigned long) r->rbx);
    kernel::Console::write(" rsp=");
    kernel::Console::writeHex((unsigned long) r->rsp);
    kernel::Console::writeLine(" ***");
    // A kernel fault is unrecoverable: do NOT iret (it would re-fault on the same instruction).
    for (;;)
        __asm__ __volatile__("hlt");
}

}  // namespace

namespace arch {

void faultInit() {
    kernel::Interrupt::registerInterruptHandler(13, &faultHandler);   // #GP
    kernel::Interrupt::registerInterruptHandler(14, &faultHandler);   // #PF
}

}  // namespace arch

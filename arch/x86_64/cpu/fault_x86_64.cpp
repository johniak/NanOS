/*
 * fault_x86_64.cpp — x86-64 CPU-exception debug handlers (#GP vector 13, #PF vector 14).
 *
 * Print the fault details (64-bit rip/cr2 via Console::writeHex(unsigned long) from Plan 2)
 * and halt, so faults are debuggable instead of triple-faulting invisibly. readCr2() comes
 * from arch/x86_64/mm/PagingControl.h (Plan 3). Process-killing (ring-3 SIGSEGV) lands when
 * the scheduler is ported.
 */
#include "Interrupt64.h"
#include "PagingControl.h"   // kernel::readCr2 (64-bit)
#include "Console.h"

namespace {

void faultHandler(kernel::Registers* r) {
    kernel::Console::write("\n*** CPU EXCEPTION vec=");
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

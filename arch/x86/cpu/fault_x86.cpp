/*
 * fault_x86.cpp — x86 CPU-exception debug handlers (#GP vector 13, #PF vector 14).
 *
 * Without these, an unhandled fault silently does nothing in the trap stub and the
 * CPU eventually triple-faults (invisible reset). These print the fault details and
 * halt, so faults are debuggable — and they are the ring-3 isolation backstop: a
 * user program touching kernel memory lands here instead of corrupting the kernel.
 */
#include "Interrupt.h"
#include "PagingControl.h"
#include "Console.h"

namespace {

void faultHandler(kernel::Registers* r) {
	kernel::Console::write("\n*** CPU EXCEPTION vec=");
	kernel::Console::writeHex((int) r->int_no);
	kernel::Console::write(" err=");
	kernel::Console::writeHex((int) r->err_code);
	kernel::Console::write(" eip=");
	kernel::Console::writeHex((int) r->eip);
	kernel::Console::write(" cs=");
	kernel::Console::writeHex((int) r->cs);
	if (r->int_no == 14) {   // #PF: CR2 holds the faulting linear address
		kernel::Console::write(" cr2=");
		kernel::Console::writeHex((int) kernel::readCr2());
	}
	kernel::Console::writeLine(" ***");
	// Do NOT iret (would re-fault on the same instruction). Halt forever.
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

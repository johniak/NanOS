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

// Append a 0-terminated "name=<16 hex>" pair into buf at *pos (bounded). Used only to build the
// one-line panic marker handed to kernel::g_panicSink — a self-contained hex formatter because
// Console::writeHex streams to the screen, not a buffer, and the sink needs a string.
void panicAppend(char* buf, int cap, int* pos, const char* name, unsigned long v) {
    for (const char* s = name; *s && *pos < cap - 1; s++) buf[(*pos)++] = *s;
    for (int shift = 60; shift >= 0 && *pos < cap - 1; shift -= 4) {
        unsigned nib = (unsigned) ((v >> shift) & 0xF);
        buf[(*pos)++] = (char) (nib < 10 ? '0' + nib : 'a' + nib - 10);
    }
    if (*pos < cap - 1) buf[(*pos)++] = ' ';
    buf[*pos] = 0;
}

void faultHandler(kernel::Registers* r) {
    if (r->cs & 3) {
        kernel::Console::write("\n[nanos: killed faulting process: vec=");
        kernel::Console::writeHex((int) r->int_no);
        if (r->int_no == 14) { kernel::Console::write(" cr2="); kernel::Console::writeHex((unsigned long) kernel::readCr2()); }
        kernel::Console::write(" rip=");
        kernel::Console::writeHex((unsigned long) r->rip);
        // rsp too: on a NULL indirect call (`call *0`) rip is 0, but [rsp] (read out-of-band with a
        // debugger/addr2line) is the caller site. We print only the register, never deref it here —
        // a process with a corrupt rsp must not fault the kernel from inside the fault handler.
        kernel::Console::write(" rsp=");
        kernel::Console::writeHex((unsigned long) r->rsp);
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
    if (r->int_no == 14 || r->int_no == 8) {
        // #PF: CR2 holds the faulting linear address. #DF: when a stack overflow triggers it, the
        // underlying #PF already latched CR2 to the guard-page address just below the blown stack —
        // so printing CR2 here pins the overflow address (and rsp confirms how far it descended).
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
    // Tee one compact line to the persistent panic sink (e.g. the i915 bring-up log on the USB
    // root), which is readable after a power-cycle even when the panel is owned by a driver whose
    // scanout no longer points at the fbcon framebuffer. Best-effort, and last: the screen print
    // above already happened, so a wedged sink can never suppress the on-screen panic.
    if (kernel::g_panicSink) {
        char line[128];
        int pos = 0;
        for (const char* s = "\n*** KERNEL EXCEPTION "; *s; s++) line[pos++] = *s;
        line[pos] = 0;
        panicAppend(line, sizeof line, &pos, "vec=", (unsigned long) r->int_no);
        panicAppend(line, sizeof line, &pos, "rip=", (unsigned long) r->rip);
        panicAppend(line, sizeof line, &pos, "rsp=", (unsigned long) r->rsp);
        if (r->int_no == 14 || r->int_no == 8)
            panicAppend(line, sizeof line, &pos, "cr2=", (unsigned long) kernel::readCr2());
        if (pos < (int) sizeof line - 1) line[pos++] = '\n';
        line[pos] = 0;
        kernel::g_panicSink(line);
    }
    // A kernel fault is unrecoverable: do NOT iret (it would re-fault on the same instruction).
    for (;;)
        __asm__ __volatile__("hlt");
}

}  // namespace

namespace kernel {
// Definition of the optional panic tee (declared in Console.h). Assigned by knx_set_panic_sink().
void (*g_panicSink)(const char* line) = nullptr;
}  // namespace kernel

namespace arch {

void faultInit() {
    kernel::Interrupt::registerInterruptHandler(8,  &faultHandler);   // #DF (IST1 stack) — makes a
                                                                     // stack overflow print rip/rsp/cr2
                                                                     // + halt visibly instead of an
                                                                     // invisible triple-fault reboot
    kernel::Interrupt::registerInterruptHandler(13, &faultHandler);   // #GP
    kernel::Interrupt::registerInterruptHandler(14, &faultHandler);   // #PF
}

}  // namespace arch

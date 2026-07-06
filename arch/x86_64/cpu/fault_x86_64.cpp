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
    kernel::Console::write(" rdi=");
    kernel::Console::writeHex((unsigned long) r->rdi);
    kernel::Console::writeLine(" ***");
    // Backtrace: scan the stack upward for values that fall inside the i915 kext's runtime .text
    // window and print them — a #GP/#PF in the driver then names its full call chain (map each via
    // the boot-log delta: link = runtime - 0x29aa1000). Cheap and read-only; the loader stack is
    // mapped, so scanning a kilobyte above rsp cannot itself fault. Collect first so the same list
    // goes to the screen AND the persistent sink below.
    unsigned long bt[12];
    int btn = 0;
    if (!(r->cs & 3)) {   // ring-0 fault only (a user fault's stack is the process's, not useful here)
        const unsigned long* sp = (const unsigned long*) r->rsp;
        for (int i = 0; i < 160 && btn < 12; i++) {
            unsigned long v = sp[i];
            if (v >= 0x2a000000UL && v < 0x2a800000UL)   // i915 kext text window (see boot log addrs)
                bt[btn++] = v;
        }
        if (btn) {
            kernel::Console::write("    bt:");
            for (int i = 0; i < btn; i++) { kernel::Console::write(" "); kernel::Console::writeHex(bt[i]); }
            kernel::Console::writeLine("");
        }
    }
    // Tee one compact line to the persistent panic sink (e.g. the i915 bring-up log on the USB
    // root), which is readable after a power-cycle even when the panel is owned by a driver whose
    // scanout no longer points at the fbcon framebuffer. Best-effort, and last: the screen print
    // above already happened, so a wedged sink can never suppress the on-screen panic.
    // Skip the FS sink for #DF (vec 8): a stack-overflow double fault runs on the small IST1 stack and
    // often means we faulted mid-operation (possibly holding the FS lock or with the heap in a partial
    // state) — the sink's ext-append (locks + allocation) could re-fault and cascade to a triple fault,
    // losing the on-screen dump too. The screen print above is the reliable channel for #DF.
    if (kernel::g_panicSink && r->int_no != 8) {
        char line[128];
        int pos = 0;
        for (const char* s = "\n*** KERNEL EXCEPTION "; *s; s++) line[pos++] = *s;
        line[pos] = 0;
        panicAppend(line, sizeof line, &pos, "vec=", (unsigned long) r->int_no);
        panicAppend(line, sizeof line, &pos, "rip=", (unsigned long) r->rip);
        panicAppend(line, sizeof line, &pos, "rsp=", (unsigned long) r->rsp);
        panicAppend(line, sizeof line, &pos, "rdi=", (unsigned long) r->rdi);
        if (r->int_no == 14 || r->int_no == 8)
            panicAppend(line, sizeof line, &pos, "cr2=", (unsigned long) kernel::readCr2());
        if (pos < (int) sizeof line - 1) line[pos++] = '\n';
        line[pos] = 0;
        kernel::g_panicSink(line);
        // Persist the kext backtrace on its own line so the driver call chain survives the power-cycle.
        if (btn) {
            char bl[256];
            int bp = 0;
            for (const char* s = "    bt:"; *s; s++) bl[bp++] = *s;
            bl[bp] = 0;
            for (int i = 0; i < btn; i++)
                panicAppend(bl, sizeof bl, &bp, " ", bt[i]);   // "<space><16 hex> "
            if (bp < (int) sizeof bl - 1) bl[bp++] = '\n';
            bl[bp] = 0;
            kernel::g_panicSink(bl);
        }
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

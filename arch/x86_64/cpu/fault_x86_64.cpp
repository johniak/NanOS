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

namespace kernel {
// Filled by the scheduler's stack-overflow canary (Scheduler.cpp); persisted here so an overflowed
// task that goes on to fault names itself on the durable sink. Empty ("\0") when no overflow was seen.
extern char g_kstackOverflowNote[];
// Last-syscall diagnostic ring (defined in SyscallDispatch.cpp) — dumped on a kernel-mode fault.
struct SyscallTrace { long nr; unsigned long a0, a1; char comm[16]; };
extern volatile SyscallTrace g_syscallRing[16];
extern volatile unsigned g_syscallRingHead;
}

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
        // Caller trace for a NULL indirect call (`call *0` faults with rip=0): the CPU pushed the
        // return address onto the stack BEFORE the jump to 0 faulted, so [rsp] is a live, mapped word
        // naming the caller site (addr2line it against the .nxe). Safe to deref: we only touch words
        // in rsp's OWN page (which must be present — the CPU just wrote to it) and only when rsp sits
        // in the user VA window, so a corrupt rsp can never fault the kernel from inside this handler.
        {
            unsigned long sp = (unsigned long) r->rsp;
            if (sp >= 0x800000UL && sp < 0x10000000UL && (sp & 7) == 0) {
                unsigned long pageEnd = (sp | 0xFFFUL) + 1;
                kernel::Console::write(" stk=");
                for (int i = 0; i < 4 && sp + 8 <= pageEnd; i++, sp += 8) {
                    kernel::Console::writeHex(*(const unsigned long*) sp);
                    kernel::Console::write(",");
                }
            }
        }
        kernel::Console::writeLine("]");
        // Tee the same line to the persistent panic sink so the nwm-death mask (a user #PF while nwm
        // owns the graphics VT — the fbcon text above is NOT scanned out) leaves cr2/rip/comm on the
        // USB log and survives the reboot. Best-effort, after the screen print. Self-limited so a
        // crash-loop can't hammer the stick. (Diagnostic — remove with the rest once root-caused.)
        static int ring3_sink_left = 16;
        if (kernel::g_panicSink && ring3_sink_left > 0) {
            ring3_sink_left--;
            char line[128];
            int pos = 0;
            for (const char* s = "\n[ring3 fault "; *s; s++) line[pos++] = *s;
            line[pos] = 0;
            panicAppend(line, sizeof line, &pos, "vec=", (unsigned long) r->int_no);
            if (r->int_no == 14)
                panicAppend(line, sizeof line, &pos, "cr2=", (unsigned long) kernel::readCr2());
            panicAppend(line, sizeof line, &pos, "rip=", (unsigned long) r->rip);
            panicAppend(line, sizeof line, &pos, "rsp=", (unsigned long) r->rsp);
            {   // caller site for a NULL indirect call (see the screen-print rationale above)
                unsigned long sp = (unsigned long) r->rsp;
                if (sp >= 0x800000UL && sp < 0x10000000UL && (sp & 7) == 0)
                    panicAppend(line, sizeof line, &pos, "ret=", *(const unsigned long*) sp);
            }
            if (kernel::Process* cp = kernel::ProcTable::current()) {
                for (const char* s = "comm="; *s && pos < (int) sizeof line - 1; s++) line[pos++] = *s;
                for (const char* s = cp->comm; *s && pos < (int) sizeof line - 2; s++) line[pos++] = *s;
            }
            if (pos < (int) sizeof line - 1) line[pos++] = '\n';
            line[pos] = 0;
            kernel::g_panicSink(line);
            // If the scheduler's canary saw a stack overflow, this SIGSEGV is likely its downstream
            // corruption — persist the culprit's name once (emit-and-clear so it prints a single time).
            if (kernel::g_kstackOverflowNote[0]) {
                kernel::g_panicSink(kernel::g_kstackOverflowNote);
                kernel::g_kstackOverflowNote[0] = 0;
            }
        }
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
    // Last-syscall ring: a kernel derail triggered mid-syscall (e.g. a user process hitting a bug in
    // a syscall handler) is best explained by the last few syscalls that ran. Print them newest-last:
    // nr + first two args + comm. Read-only global (SyscallDispatch.cpp); safe from any fault context.
    {
        unsigned head = kernel::g_syscallRingHead;
        kernel::Console::writeLine("    last syscalls (nr a0 a1 comm), newest last:");
        for (int i = 8; i >= 1; i--) {
            unsigned idx = (head - (unsigned) i) & 15;
            kernel::Console::write("      nr="); kernel::Console::writeHex((unsigned long) kernel::g_syscallRing[idx].nr);
            kernel::Console::write(" a0="); kernel::Console::writeHex(kernel::g_syscallRing[idx].a0);
            kernel::Console::write(" a1="); kernel::Console::writeHex(kernel::g_syscallRing[idx].a1);
            kernel::Console::write(" "); kernel::Console::write((const char*) kernel::g_syscallRing[idx].comm);
            kernel::Console::writeLine("");
        }
    }
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
    // #DF (vec 8, the stack-overflow double fault) was previously SKIPPED here: it runs on the small
    // IST1 stack and often faulted mid-operation (FS lock held / heap partial), so the sink's ext-append
    // could re-fault and cascade to a triple fault, losing the on-screen dump too. But while i915 owns
    // the panel that on-screen dump is invisible (fbcon is not scanned out), so a persisted line is the
    // ONLY evidence — attempt it ONCE behind a latch: a re-fault then triple-faults, which a #DF was
    // heading toward anyway; the latch stops infinite sink re-entry if we somehow survive.
    static bool df_sink_tried = false;
    bool do_sink = (r->int_no != 8) || !df_sink_tried;
    if (r->int_no == 8)
        df_sink_tried = true;
    if (kernel::g_panicSink && do_sink) {
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
        // If the scheduler's canary saw a stack overflow earlier, this ring-0 fault is very likely its
        // downstream corruption — name the culprit task on the durable sink (emit-and-clear, once).
        if (kernel::g_kstackOverflowNote[0]) {
            kernel::g_panicSink(kernel::g_kstackOverflowNote);
            kernel::g_kstackOverflowNote[0] = 0;
        }
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

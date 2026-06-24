/*
 * smp_x86.cpp — i686 uniprocessor stub for <arch/smp.h>.
 *
 * The 32-bit arch is frozen and single-CPU; it never brings up application processors. These
 * no-op definitions keep the i686 kernel linking against the MI SMP callers (the BKL, the
 * scheduler). One CPU, index 0, no IPIs.
 */
#include <arch/smp.h>

namespace arch {

int  smpInit()     { return 1; }
int  smpCpuCount() { return 1; }
int  smpThisCpu()  { return 0; }
void smpBringUpAPs(const uint8_t*, int) {}
void smpSetApEntry(ApEntry) {}
void smpSendIpi(int, uint8_t) {}
void smpTlbShootdown(uint64_t) {}
void smpPollShootdown() {}

}  // namespace arch

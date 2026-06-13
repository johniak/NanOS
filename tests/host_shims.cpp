// Host implementations of freestanding kernel primitives, so the storage stack
// can be compiled and run natively under the test harness.
//
// Memory (malloc/free/realloc, global new/delete) and string.h come from libc by
// simply NOT linking memory_manager.cpp / string_funcs.cpp into the test binary.
// The real MI Console.cpp is compiled in the test build; only the arch console
// SINK (<arch/console.h>) needs a host stand-in — it normally writes VGA memory.
#include <arch/console.h>
#include <arch/input.h>
#include <arch/cpu.h>
#include "memory_manager.h"
#include "SynthFs.h"
#include <cstdio>

// memory_manager.h declares malloc/free/realloc/calloc with C++ linkage (no
// extern "C"), so libc's C-linkage versions don't satisfy them. Provide the
// matching C++ symbols here, forwarding to the libc allocator via builtins
// (no <cstdlib> include, so no conflicting C-linkage redeclaration).
void* malloc(size_t n) { return __builtin_malloc(n); }
void free(void* p) { __builtin_free(p); }
void* realloc(void* p, size_t n) { return __builtin_realloc(p, n); }
void* calloc(size_t a, size_t b) { return __builtin_calloc(a, b); }

// Arch console sink stand-in: route glyphs to stdout, ignore cursor/clear.
namespace arch {
void consolePutChar(char c) { putchar(c); }
void consoleClear() {}
void consoleSetCursor(unsigned, unsigned) {}
void consoleInit() {}

// No real keyboard under the host harness: console reads are immediate EOF (this
// preserves the pre-blocking-stdin behaviour the Syscalls tests expect).
int inputRead(char*, unsigned, int) { return 0; }
void inputSetRaw(int) {}

// Scheduler arch primitives are hardware (context switch / timer); the host harness
// only exercises the pure round-robin logic, so these are no-op stubs.
unsigned archTaskBootstrap(unsigned char*, unsigned) { return 0; }
unsigned archKernelCr3() { return 0; }
void archTimerInit(unsigned) {}
void setKernelStack(unsigned) {}
void halt_or_hlt() {}
unsigned long cpuIrqSave() { return 0; }   // no interrupts on the host harness
void cpuIrqRestore(unsigned long) {}
}   // namespace arch

// The Scheduler's sleep primitives consult this (don't block through a pending signal); the
// host harness never has one, so the sleep paths behave exactly as before under test.
namespace kernel { bool hasPendingSignalCurrent() { return false; } }
namespace arch {

// CPUID is x86-only; under the host harness fill a representative CpuInfo so the
// /proc/cpuinfo generator links and renders. (The pure cpuinfoString renderer is tested
// directly with a fabricated CpuInfo.)
void cpuIdentify(CpuInfo* out) {
	const char* v = "HostTestCPU"; int i = 0;
	for (; v[i]; i++) out->vendor[i] = v[i];
	out->vendor[i] = 0;
	out->brand[0] = 0;
	out->family = 6; out->model = 0; out->stepping = 0; out->khz = 0;
	const char* f = "fpu tsc"; int j = 0;
	for (; f[j]; j++) out->flags[j] = f[j];
	out->flags[j] = 0;
}

// No RTC under the host harness; a fixed plausible 2026 epoch keeps /proc/stat's btime
// renderable in tests.
unsigned rtcEpoch() { return 1781000000u; }

// Hardware entropy contract (<arch/random.h>). The host harness has no RDRAND/RDTSC: report "no
// hardware RNG" and a fixed tick. The Csprng tests seed the class directly with known vectors and
// never call csprngKernelSeed(), so these only need to link — they need not be random.
bool archHwRandom(unsigned*) { return false; }
unsigned archEntropyTick() { return 0; }
}
extern "C" void archContextSwitch(unsigned*, unsigned) {}   // C linkage (see arch/sched.h)

// /proc/meminfo data sources live in the kernel (Kernel.cpp, not in the test build);
// stub them with fixed figures so SynthFs links and the meminfo file is readable.
namespace kernel {
unsigned sysMemTotalKb() { return 131072; }   // 128 MiB
unsigned sysMemFreeKb()  { return 120000; }
unsigned sysHeapTotalKb() { return 5120; }
unsigned sysHeapFreeKb()  { return 5000; }
}

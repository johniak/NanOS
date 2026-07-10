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
#include <cstdint>   // uintptr_t/uint64_t: match the widened <arch/sched.h> contract

// memory_manager.h declares malloc/free/realloc/calloc with C++ linkage (no
// extern "C"), so libc's C-linkage versions don't satisfy them. Provide the
// matching C++ symbols here, forwarding to the libc allocator via builtins
// (no <cstdlib> include, so no conflicting C-linkage redeclaration).
void* malloc(size_t n) { return __builtin_malloc(n); }
void free(void* p) { __builtin_free(p); }
void* realloc(void* p, size_t n) { return __builtin_realloc(p, n); }
void* calloc(size_t a, size_t b) { return __builtin_calloc(a, b); }

// LinuxKPI shim imports: the kext resolves these to the kernel export table; under the
// host harness they forward to libc. knx_uptime_us is supplied by the time test (its only
// consumer), so that test can drive a deterministic clock.
extern "C" void *knx_malloc(unsigned n) { return __builtin_malloc(n); }
extern "C" void  knx_free(void *p) { __builtin_free(p); }
extern "C" void  knx_log(const char *s) { (void)s; }
// MSI registration stub: kpi_irq.c's lkpi_irq_bind_msi references it; the irq-table doctest
// exercises request_irq/free_irq/dispatch directly and never needs a real MSI, so return failure.
extern "C" int knx_register_msi(unsigned char, unsigned char, unsigned char,
                                void (*)(void *), void *) { return -1; }
// Kernel-thread facility for the kpi_kthread doctest: no real threads on the host — spawn returns a
// non-null dummy handle (the worker body is never auto-run; the test drives lkpi_wq_drain itself),
// and run_after_scheduler fires immediately so lkpi_wq_init flips to async in the test.
extern "C" void *knx_thread_spawn(void (*)(void *), void *, const char *) { return (void *)1; }
extern "C" int   knx_thread_should_stop(void) { return 0; }
extern "C" void  knx_thread_stop(void *) {}
extern "C" void  knx_thread_yield(void) {}
extern "C" void  knx_thread_msleep(unsigned) {}
// One constant identity: the host doctest is single-threaded, so the cross-core gate always sees
// the same "task" and recurses instead of ever spinning.
extern "C" void *knx_cur_task(void) { return (void *)1; }

// Spinlock wedge tripwire (kernel/Spinlock.h): defined by Kernel.cpp on the target, which the
// host test link excludes — stub it unarmed here so contended test spins stay silent.
namespace kernel { void (*g_spinStallSink)(const void *ra) = nullptr; }
// Cross-core gate (linuxkpi/kpi_misc.c): some doctest binaries link kpi_rcu/kpi_fence without
// kpi_misc. WEAK no-ops satisfy those links; when kpi_misc.c IS linked its strong (and on the
// single-threaded host, trivially recursive) definitions win.
extern "C" __attribute__((weak)) void lkpi_gate_enter(void) {}
extern "C" __attribute__((weak)) void lkpi_gate_exit(void) {}
extern "C" __attribute__((weak)) int  lkpi_gate_try_enter(void) { return 1; }
extern "C" void  knx_rcu_synchronize(void) {}   // host doctest is single-threaded: a grace period is instant
extern "C" void  knx_run_after_scheduler(void (*fn)(void)) { if (fn) fn(); }
// knx_file_read stand-in for the request_firmware doctest: a single settable fake file. The test
// registers a path+blob via lkpi_test_set_file, then request_firmware resolves that exact path.
static const char* g_fakePath = 0;
static const void* g_fakeData = 0;
static unsigned long g_fakeSize = 0;
extern "C" void lkpi_test_set_file(const char* path, const void* data, unsigned long size) {
	g_fakePath = path; g_fakeData = data; g_fakeSize = size;
}
extern "C" int knx_file_read(const char* path, void* buf, unsigned long max, unsigned long* out_len) {
	if (!g_fakePath || !path) return -2;
	const char* a = path; const char* b = g_fakePath;
	while (*a && *a == *b) { a++; b++; }
	if (*a != *b) return -2;                          // path mismatch -> ENOENT
	if (!buf) { if (out_len) *out_len = g_fakeSize; return 0; }
	unsigned long n = g_fakeSize < max ? g_fakeSize : max;
	for (unsigned long i = 0; i < n; i++) ((char*)buf)[i] = ((const char*)g_fakeData)[i];
	if (out_len) *out_len = n;
	return 0;
}
// kpi_fence.c (not in the host test) normally provides this; the wq test doesn't need the pump hook.
extern "C" void  lkpi_set_wq_pump(void (*)(void)) {}

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
uintptr_t archTaskBootstrap(unsigned char*, uint64_t) { return 0; }
uint64_t archKernelCr3() { return 0; }
void archTimerInit(unsigned) {}
void setKernelStack(uintptr_t) {}
void archLoadThreadTls(unsigned) {}   // TLS descriptor reload is hardware (GDT + %gs): no-op on host
void halt_or_hlt() {}
unsigned long cpuIrqSave() { return 0; }   // no interrupts on the host harness
void cpuIrqRestore(unsigned long) {}
void cpuRelax() {}                          // spin-relax hint is a no-op on the host harness
int  smpThisCpu() { return 0; }             // host harness is single-threaded -> always CPU 0
int  smpCpuCount() { return 1; }            // host harness is uniprocessor
void smpPollShootdown() {}                  // no cross-CPU TLB shootdowns on the host harness
}   // namespace arch

// The Scheduler's sleep primitives consult this (don't block through a pending signal); the
// host harness never has one, so the sleep paths behave exactly as before under test.
namespace kernel { bool hasPendingSignalCurrent() { return false; } }
// consoleSignalGroup lives in Exec.cpp (not linked into the host suite); the VT console's
// cooked control-key path references it. The harness never exercises a real signal, so a no-op
// stub satisfies the link.
namespace kernel { void consoleSignalGroup(int, int) {} }
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
extern "C" void archContextSwitch(uintptr_t*, uintptr_t, void*, void*) {}   // C linkage (see arch/sched.h)
namespace arch {
void archFpuCapture(void*) {}          // host: no live FPU context to snapshot
void archFpuLoad(const void*) {}
}

// /proc/meminfo data sources live in the kernel (Kernel.cpp, not in the test build);
// stub them with fixed figures so SynthFs links and the meminfo file is readable.
namespace kernel {
unsigned sysMemTotalKb() { return 131072; }   // 128 MiB
unsigned sysMemFreeKb()  { return 120000; }
unsigned sysHeapTotalKb() { return 5120; }
unsigned sysHeapFreeKb()  { return 5000; }
}

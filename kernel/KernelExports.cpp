#include "KernelExports.h"
#include "SynthFs.h"
#include "CharDevice.h"
#include "Console.h"
#include "Scheduler.h"
#include "memory_manager.h"
#include <arch/irq.h>
#include <arch/input.h>
#include <string.h>

namespace kernel {

static SynthFs* g_root = 0;
static int g_nextInput = 1;   // /dev/input0 is the kernel-side keyboard evdev; modules get >=1

void kernelExportsInit(SynthFs* root) { g_root = root; }

// ---- the exported kernel API (stable C ABI; bodies are thin wrappers over kernel internals) ----
extern "C" {

void* knx_malloc(unsigned n)              { return malloc(n); }
void  knx_free(void* p)                   { free(p); }
void  knx_log(const char* s)              { Console::write(s); }
unsigned long long knx_uptime_us(void)    { return (unsigned long long) Scheduler::ticks() * 1000ull; }

// Register a device IRQ-line handler. The module sees an opaque frame (void*); arch's
// IrqHandler takes its TrapFrame*, same calling convention, so the cast is safe.
void knx_register_irq(int irq, void (*h)(void*)) {
	arch::registerIrqHandler((unsigned) irq, (arch::IrqHandler) h);
}

// Push one PS/2 scancode into the console/evdev input layer (used by the keyboard kext).
void knx_feed_scancode(unsigned char sc)  { arch::inputFeedScancode(sc); }

// Publish a CharDevice as the next /dev/input<N> (N auto-assigned; input0 = keyboard) — like
// Linux's dynamically-numbered /dev/input/eventN. Returns the assigned N, or -1.
int knx_add_input_dev(CharDevice* dev) {
	if (!g_root || !dev)
		return -1;
	int n = g_nextInput++;
	char name[16];
	int i = 0;
	for (const char* b = "input"; *b; b++)
		name[i++] = *b;
	if (n >= 10)
		name[i++] = (char) ('0' + n / 10);
	name[i++] = (char) ('0' + n % 10);
	name[i] = 0;
	g_root->addChar(g_root->dev(), name, dev, 0444);
	return n;
}

}  // extern "C"

// ---- the resolver table, generated from kexports.def (single source of truth) ----
struct KExport { const char* name; void* fn; };
static const KExport g_exports[] = {
#define KX(n) { #n, (void*) n },
#include "kexports.def"
#undef KX
};

void* kernelResolveSym(const char* name, const char* lib) {
	(void) lib;   // all kernel exports share the one implicit "kernel" namespace
	for (unsigned i = 0; i < sizeof(g_exports) / sizeof(g_exports[0]); i++)
		if (strcmp(g_exports[i].name, name) == 0)
			return g_exports[i].fn;
	return 0;
}

}  // namespace kernel

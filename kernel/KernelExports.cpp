#include "KernelExports.h"
#include "SynthFs.h"
#include "Vfs.h"            // knx_file_read: read firmware blobs through the VFS
#include "String.h"         // Vfs::read takes a String path
#include "CharDevice.h"
#include "Console.h"
#include "Scheduler.h"
#include "Pci.h"
#include "MsiRouter.h"   // MSI/MSI-X capability walk + programming (host-tested)
#include "knx_net.h"   // knx_map_mmio / knx_dma_alloc / knx_add_net_dev / knx_netif_rx (NetCore.cpp)
#include "memory_manager.h"
#include "Fbdev.h"          // FbInfo
#include "Fb0Device.h"      // /dev/fb0 over a kext framebuffer
#include "DrmDevice.h"      // /dev/dri/card0 + renderD128 forwarder
#include "knx_drm_node.h"   // knx_drm_ops table (kext/virtio_gpu)
#include "Process.h"        // ProcTable::current() for knx_getpid / syscallCurrentPid
#include "Framebuffer.h"    // FbSurface
#include "vt/VtManager.h"   // VtManager + g_vtmgr + kVtCount (graphics console over a kext fb)
#include "VtTty.h"          // /dev/ttyN
#include "SignalDispatch.h" // signalSend (VT_SETMODE handshake)
#include <arch/irq.h>
#include <arch/input.h>
#include <arch/console.h>   // consoleSerialOut
#include <arch/bootinfo.h>  // bootFramebuffer (mirror source for a display kext)
#include <stdint.h>
#include <string.h>

namespace kernel {

static SynthFs* g_root = 0;
static Vfs* g_kexVfs = 0;     // the VFS knx_file_read reads firmware blobs through
static int g_nextInput = 1;   // /dev/input0 is the kernel-side keyboard evdev; modules get >=1

void kernelExportsInit(SynthFs* root) { g_root = root; }
void kernelExportsSetVfs(Vfs* vfs) { g_kexVfs = vfs; }

// LAPIC accessors (arch/x86_64/cpu/lapic_x86_64.cpp) used by knx_register_msi below.
uint8_t lapicId();
int     lapicAllocVector();

// DRM node helpers (kernel:: linkage; defined after the extern "C" block). Forward-declared here
// so the C-ABI knx_getpid/knx_drm_register wrappers inside that block can call them.
int  syscallCurrentPid();
void drmNodesRegister(const struct knx_drm_ops* ops);

// MSI handler slot + trampoline (Phase 1: one active NIC vector). arch::registerTrapHandler installs
// msiTrampoline on the LAPIC vector msiSetup allocated; it forwards to the module's handler + ctx.
static void (*g_msiHandler)(void*) = 0;
static void* g_msiCtx = 0;
static void msiTrampoline(arch::TrapFrame*) { if (g_msiHandler) g_msiHandler(g_msiCtx); }

// ---- the exported kernel API (stable C ABI; bodies are thin wrappers over kernel internals) ----
extern "C" {

void* knx_malloc(unsigned n)              { return malloc(n); }
void  knx_free(void* p)                   { free(p); }
void  knx_log(const char* s)              { Console::write(s); }
unsigned long long knx_uptime_us(void)    { return (unsigned long long) Scheduler::ticks() * 1000ull; }

// Read a whole file through the VFS (the same path the kext/init loaders use). With buf==0, report
// the file size in *out_len and return 0 (so a caller can size a buffer, then read). Otherwise copy
// up to `max` bytes into buf and set *out_len to the bytes read. Returns 0 on success, <0 on error
// (-2 = ENOENT/unreadable). Used by request_firmware; firmware blobs are optional on Gen9 i915.
int knx_file_read(const char* path, void* buf, unsigned long max, unsigned long* out_len) {
	if (!g_kexVfs || !path) return -2;
	String p((char*) path);
	FileStat st;
	if (g_kexVfs->stat(p, st) < 0) return -2;
	if (!buf) { if (out_len) *out_len = st.size; return 0; }
	unsigned want = st.size < max ? st.size : (unsigned) max;
	int n = g_kexVfs->read(p, want, 0, buf);
	if (n < 0) return -2;
	if (out_len) *out_len = (unsigned long) n;
	return 0;
}

// Real RCU grace period (LinuxKPI synchronize_rcu). See Scheduler::rcuSynchronize.
void knx_rcu_synchronize(void)            { Scheduler::rcuSynchronize(); }

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

// ---- DRM nodes: /dev/dri/card0 + /dev/dri/renderD128 ----
// The virtio_gpu kext owns the vendored DRM stack; it registers a knx_drm_ops table and two
// DrmDevice char devices forward SYS_ioctl / SYS_mmap(offset) into it (see DrmDevice + Task 4/5).
// The C-ABI exports below are thin wrappers over the kernel::-linkage helpers (defined after the
// extern "C" block), so DrmDevice.cpp (which calls kernel::syscallCurrentPid) links correctly.
int  knx_getpid(void)                            { return syscallCurrentPid(); }
void knx_drm_register(const struct knx_drm_ops* ops) { drmNodesRegister(ops); }

// ---- PCI access for driver modules (the e1000 NIC kext binds its device through these) ----
// The (bus,dev,func) triple is the stable handle; a driver finds it once via knx_pci_find then
// reads BARs / IRQ and flips bus-mastering. Stateless thin wrappers over kernel::Pci.

int knx_pci_find(uint16_t vendor, uint16_t device, uint8_t* bus, uint8_t* dev, uint8_t* func) {
	PciDevice d;
	if (!Pci::find(vendor, device, d))
		return 0;
	if (bus)  *bus = d.bus;
	if (dev)  *dev = d.dev;
	if (func) *func = d.func;
	return 1;
}
uint32_t knx_pci_bar(uint8_t bus, uint8_t dev, uint8_t func, int n) {
	PciDevice d;
	if (n < 0 || n > 5 || !Pci::probe(bus, dev, func, d)) return 0;
	return d.bar[n].addr;
}
uint32_t knx_pci_bar_size(uint8_t bus, uint8_t dev, uint8_t func, int n) {
	PciDevice d;
	if (n < 0 || n > 5 || !Pci::probe(bus, dev, func, d)) return 0;
	return d.bar[n].size;
}
int knx_pci_bar_is_io(uint8_t bus, uint8_t dev, uint8_t func, int n) {
	PciDevice d;
	if (n < 0 || n > 5 || !Pci::probe(bus, dev, func, d)) return 0;
	return d.bar[n].isIo ? 1 : 0;
}
uint8_t knx_pci_irq(uint8_t bus, uint8_t dev, uint8_t func) {
	return Pci::read8(bus, dev, func, PCI_IRQ_LINE);
}
void knx_pci_enable_bus_master(uint8_t bus, uint8_t dev, uint8_t func) {
	PciDevice d; d.bus = bus; d.dev = dev; d.func = func;
	Pci::enableBusMaster(d);
	Pci::enableMemSpace(d);    // memory-mapped NICs need MEM decode too
}
uint32_t knx_pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
	return Pci::read32(bus, dev, func, off);
}
void knx_pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t v) {
	Pci::write32(bus, dev, func, off, v);
}

// Set up MSI-X (preferred) or MSI for a PCI function: walk the cap list, allocate a LAPIC vector,
// program the capability and install `h(ctx)` on that vector. Returns 0, or <0 if neither MSI-X nor
// MSI is available (the driver then falls back to legacy INTx via knx_register_irq).
int knx_register_msi(uint8_t bus, uint8_t dev, uint8_t func, void (*h)(void*), void* ctx) {
	MsiEnv env;
	env.cfgRead     = [](uint8_t b, uint8_t d, uint8_t f, uint8_t o) { return Pci::read32(b, d, f, o); };
	env.cfgWrite    = [](uint8_t b, uint8_t d, uint8_t f, uint8_t o, uint32_t v) { Pci::write32(b, d, f, o, v); };
	env.mapMmio     = [](uint32_t p, uint32_t l) -> void* { return knx_map_mmio(p, l); };
	env.allocVector = []() { return lapicAllocVector(); };
	env.lapicId     = []() { return lapicId(); };
	MsiResult r = msiSetup(env, bus, dev, func);
	if (r.kind == MSI_NONE)
		return -1;
	g_msiHandler = h;
	g_msiCtx = ctx;
	arch::registerTrapHandler((unsigned) r.vector, msiTrampoline);
	return 0;
}

// ---- knx_fb_set_backing: adopt a kext-provided framebuffer ----
// A display kext (e.g. virtio_gpu) that owns its own scanout buffer calls this to make that
// buffer the system framebuffer. If the bootloader gave no framebuffer (so the kernel skipped
// its graphics bring-up), we build the VT console + /dev/tty1..7 + /dev/fb0 over the kext
// buffer here — this runs during loadAllKexts, BEFORE the scheduler starts init, so init's
// spawn_nwm finds /dev/fb0 + /dev/tty7 and brings up the desktop. A periodic kernel thread
// calls the kext's flush callback so whatever the console/nwm draw is presented to the device.
void knx_fb_start_present(void (*flush)(void));   // fwd (defined below; used by set_backing)
static void (*g_fbFlush)(void) = 0;
static void fbFlushBody() {
	for (;;) {
		unsigned t = Scheduler::ticks();
		if (g_fbFlush)
			g_fbFlush();
		Scheduler::sleepUntil(t + 33);  // ~30 fps; leaves the CPU to userspace (single-core safe)
	}
}

void knx_fb_set_backing(uint64_t phys, uint32_t pitch, uint32_t w, uint32_t h,
                        uint8_t bpp, void (*flush)(void)) {
	if (!g_root)
		return;

	// Bring up the VT graphics stack over the kext fb if the bootloader gave us none.
	if (!g_vtmgr) {
		FbSurface s = { (uint8_t*) (uintptr_t) phys, pitch, w, h, bpp };
		VtManager* vtmgr = new VtManager();
		vtmgr->init(s, [](int pid, int sig) { signalSend(pid, sig); }, arch::consoleSerialOut);
		g_vtmgr = vtmgr;
		for (int i = 1; i <= kVtCount; i++) {
			char nm[6] = { 't', 't', 'y', (char) ('0' + i), 0, 0 };
			g_root->addChar(g_root->dev(), nm, new VtTty(i), 0666);
		}
		g_root->addChar(g_root->dev(), "tty0", new VtTty(0), 0666);
		g_root->addChar(g_root->dev(), "console", new VtTty(1), 0600);
	}

	// Expose /dev/fb0 over the kext buffer (nwm mmaps this).
	FbInfo info = { phys, pitch, w, h, bpp };
	g_root->addChar(g_root->dev(), "fb0", new Fb0Device(info), 0666);

	// Periodic present thread (task id 5).
	knx_fb_start_present(flush);
}

// Register the present callback. The thread itself is spawned later by fbStartPresentThread(),
// called from Kernel::start AFTER Scheduler::init() — kexts load BEFORE the scheduler exists,
// so creating the task here would be wiped by Scheduler::init().
void knx_fb_start_present(void (*flush)(void)) {
	g_fbFlush = flush;
}

// Report the bootloader-provided framebuffer (vesafb/GOP), so a display kext can MIRROR the
// already-working console/desktop onto its device. Returns 1 if a framebuffer exists, else 0.
int knx_boot_fb(uint64_t* addr, uint32_t* pitch, uint32_t* w, uint32_t* h, uint8_t* bpp) {
	const arch::BootFramebuffer* fb = arch::bootFramebuffer();
	if (!fb)
		return 0;
	if (addr)  *addr  = fb->addr;
	if (pitch) *pitch = fb->pitch;
	if (w)     *w     = fb->width;
	if (h)     *h     = fb->height;
	if (bpp)   *bpp   = fb->bpp;
	return 1;
}

// ---- kernel threads for LinuxKPI (kthreads + async workqueue workers, Task 3) ----
// A knx-spawned thread carries (fn,arg,stop,done) via the Task's opaque arg. kthread_should_stop
// reads the RUNNING task's flag; knx_thread_stop sets it and waits for the body to leave its loop.
// Kernel threads are not processes (no Process bound) — pure scheduler tasks like the present thread.
struct KnxThread { void (*fn)(void*); void* arg; volatile int stop; volatile int done; };
static int g_knxThreadId = 6;   // informational task id base (present thread is 5)

static void knxThreadTrampoline() {
	KnxThread* k = (KnxThread*) Scheduler::current()->arg;
	if (k) { k->fn(k->arg); k->done = 1; }
}

// Spawn a kernel thread running fn(arg). Returns an opaque handle for knx_thread_stop, or 0.
void* knx_thread_spawn(void (*fn)(void*), void* arg, const char* name) {
	(void) name;
	KnxThread* k = (KnxThread*) malloc(sizeof(KnxThread));
	if (!k)
		return 0;
	k->fn = fn; k->arg = arg; k->stop = 0; k->done = 0;
	if (!Scheduler::create(knxThreadTrampoline, k, g_knxThreadId++)) { free(k); return 0; }
	return k;
}

// True inside a knx thread whose stop flag was set (kthread_should_stop). 0 for non-knx tasks.
int knx_thread_should_stop(void) {
	Task* t = Scheduler::current();
	KnxThread* k = t ? (KnxThread*) t->arg : 0;
	return k ? k->stop : 0;
}

// Ask a knx thread to stop and wait for it to exit its loop (kthread_stop). Frees the handle.
void knx_thread_stop(void* handle) {
	KnxThread* k = (KnxThread*) handle;
	if (!k)
		return;
	k->stop = 1;
	while (!k->done)
		Scheduler::yield();   // cooperative: let the worker reach its next stop-check
	free(k);
}

// Yield the CPU (used by worker/timer loops between polls).
void knx_thread_yield(void) { Scheduler::yield(); }

// Sleep ~ms milliseconds (1 tick = 1 ms). An IDLE worker/timer thread must sleep, not spin-yield:
// spin-yielding saturates the run queue and starves latency-sensitive work (VT redraw, input) — the
// present thread sleeps the same way. A parked task consumes no CPU until its tick.
void knx_thread_msleep(unsigned ms) { Scheduler::sleepUntil(Scheduler::ticks() + (ms ? ms : 1)); }

// Register a callback to run once, AFTER the scheduler is up (kexts load pre-scheduler, so anything
// that must spawn kernel threads — workqueue/timer workers — defers here, like the present thread).
// Kernel::start calls runAfterSchedulerHooks() right after Scheduler::init + fbStartPresentThread.
static void (*g_afterSched[8])(void);
static int   g_afterSchedN = 0;
void knx_run_after_scheduler(void (*fn)(void)) {
	if (fn && g_afterSchedN < 8)
		g_afterSched[g_afterSchedN++] = fn;
}

}  // extern "C"

// Invoke every knx_run_after_scheduler callback (kernel linkage; called from Kernel::start).
void runAfterSchedulerHooks() {
	for (int i = 0; i < g_afterSchedN; i++)
		if (g_afterSched[i])
			g_afterSched[i]();
}

// kernel:: linkage (NOT extern "C") — DrmDevice.cpp references kernel::syscallCurrentPid, and
// DrmDevice.h declares kernel::drmNodesRegister.
int syscallCurrentPid() {
	Process* p = ProcTable::current();
	return p ? p->pid : 0;
}
void drmNodesRegister(const struct knx_drm_ops* ops) {
	if (!g_root)
		return;
	SynthNode* dri = g_root->addDir(g_root->dev(), "dri");
	if (!dri)
		return;
	g_root->addChar(dri, "card0",      new DrmDevice(ops, KNX_DRM_NODE_PRIMARY), 0666);
	g_root->addChar(dri, "renderD128", new DrmDevice(ops, KNX_DRM_NODE_RENDER),  0666);
}

// Spawn the framebuffer present thread, if a display kext registered a flush callback. Called
// from Kernel::start AFTER Scheduler::init() (kexts load before the scheduler exists).
void fbStartPresentThread() {
	if (g_fbFlush)
		Scheduler::create(fbFlushBody, 5);
}

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

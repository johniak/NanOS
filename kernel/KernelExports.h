/*
 * KernelExports.h — the symbols the kernel exports to loadable modules (nkext), à la Linux
 * EXPORT_SYMBOL. A module imports these by name; the loader binds them via kernelResolveSym.
 * The list lives in kexports.def (single source of truth) and the wrappers are extern "C"
 * (stable ABI, no C++ mangling across the kernel↔kext boundary).
 */
#pragma once

namespace kernel {

class SynthFs;
struct CharDevice;

// Wire the SynthFs root used by knx_add_input_dev (called once from Kernel::start).
void kernelExportsInit(SynthFs* root);

// Spawn the framebuffer present thread if a display kext registered one (knx_fb_set_backing /
// knx_fb_start_present). Call from Kernel::start AFTER Scheduler::init().
void fbStartPresentThread();

// Resolve an exported kernel symbol by name (the kext loader's import resolver). The `lib`
// argument is the importing module's declared source library ("kernel"); ignored — every
// kernel export is in the one implicit namespace. Returns 0 if not exported.
void* kernelResolveSym(const char* name, const char* lib);

}  // namespace kernel

#ifndef _LKPI_FPU_API_H
#define _LKPI_FPU_API_H
/* Real kernel_fpu_begin/end (NOT no-ops): in NanOS kernel context the live FPU/SSE
 * registers belong to the CALLING USER TASK (kernel + kexts are -mno-sse), so any bracketed
 * FPU use (i915/drm movntdqa WC-memcpy) must save/restore them or it silently corrupts the
 * user's XMM state outside the context-switch fxsave points. Backed by per-CPU depth-counted
 * fxsave/fxrstor in KernelExports.cpp. Linux contract holds: no sleeping in between. */
void knx_fpu_begin(void);
void knx_fpu_end(void);
static inline void kernel_fpu_begin(void) { knx_fpu_begin(); }
static inline void kernel_fpu_end(void)   { knx_fpu_end(); }
#endif

/*
 * nx_tls.c — thread-local storage for NanOS user programs (main thread).
 *
 * NanOS programs historically used no `__thread` variables: errno lives in the TCB (tls.c's
 * __errno_location), and the old __nx_init_tls installed only a bare self-pointing TCB. Mesa is
 * the first .nxe with real TLS — `_egl_TLS` (EGL per-thread state, the first thing
 * eglGetPlatformDisplay touches), `_glapi_tls_Dispatch`/`_glapi_tls_Context` (shared-glapi
 * dispatch, read on every GL call). Their accesses (both local-exec %fs:negoffset and
 * local-dynamic __tls_get_addr) need a real per-thread TLS block, which nothing set up → an early
 * page fault in EGL init.
 *
 * This vendors musl 1.2.5's __copy_tls + __tls_get_addr VERBATIM (x86_64 = variant II: the TLS
 * block sits just below the thread pointer, TCB at %fs:0), and adds __nx_init_main_tls(): it
 * discovers the TLS template from LINKER symbols (arch/x86_64/user-nx.ld — the flat .nx image has
 * no ELF program headers, so musl's auxv/PT_TLS discovery can't run), runs musl's exact
 * template-size math, builds the block with __copy_tls, and installs the thread pointer via the
 * port's __set_thread_area (arch_prctl ARCH_SET_FS). tls.c's __nx_init_tls calls it before main.
 *
 * Programs with NO TLS return 0 here and keep the bare-TCB path in tls.c.
 */
#define SYSCALL_NO_TLS 1
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include "pthread_impl.h"
#include "libc.h"

/* ---- musl 1.2.5 src/thread/__tls_get_addr.c (verbatim) --------------------------------------- */
void *__tls_get_addr(tls_mod_off_t *v)
{
	pthread_t self = __pthread_self();
	return (void *)(self->dtv[v[0]] + v[1]);
}

/* ---- musl 1.2.5 src/env/__init_tls.c: builtin_tls, MIN_TLS_ALIGN, main_tls, __copy_tls -------- */
static struct builtin_tls {
	char c;
	struct pthread pt;
	void *space[128];   /* NanOS: sized so the main TLS block fits without an early mmap */
} builtin_tls[1];
#define MIN_TLS_ALIGN offsetof(struct builtin_tls, pt)

static struct tls_module main_tls;

void *__copy_tls(unsigned char *mem)
{
	pthread_t td;
	struct tls_module *p;
	size_t i;
	uintptr_t *dtv;

#ifdef TLS_ABOVE_TP
	dtv = (uintptr_t*)(mem + libc.tls_size) - (libc.tls_cnt + 1);

	mem += -((uintptr_t)mem + sizeof(struct pthread)) & (libc.tls_align-1);
	td = (pthread_t)mem;
	mem += sizeof(struct pthread);

	for (i=1, p=libc.tls_head; p; i++, p=p->next) {
		dtv[i] = (uintptr_t)(mem + p->offset) + DTP_OFFSET;
		memcpy(mem + p->offset, p->image, p->len);
	}
#else
	dtv = (uintptr_t *)mem;

	mem += libc.tls_size - sizeof(struct pthread);
	mem -= (uintptr_t)mem & (libc.tls_align-1);
	td = (pthread_t)mem;

	for (i=1, p=libc.tls_head; p; i++, p=p->next) {
		dtv[i] = (uintptr_t)(mem - p->offset) + DTP_OFFSET;
		memcpy(mem - p->offset, p->image, p->len);
	}
#endif
	dtv[0] = libc.tls_cnt;
	td->dtv = dtv;
	return td;
}

/* ---- NanOS: template (from crt0, via linker symbols) + install (adapts musl static_init_tls) --- */
int __set_thread_area(void *);     /* port asm (__set_thread_area64.s): arch_prctl ARCH_SET_FS */

/* Build + install the main thread's TLS block. The template comes from crt0 (image = .tdata start,
 * filesz = .tdata size, memsz = .tdata+.tbss, align = segment alignment) — those symbols are
 * PER-PROGRAM (user-nx.ld) and unreachable from libc.ndl, so they arrive as arguments. Returns the
 * TCB (thread pointer) on success, or 0 if the program has no TLS (caller keeps the bare-TCB
 * fallback). */
void *__nx_init_main_tls(void *image, size_t filesz, size_t memsz, size_t align)
{
	if (!memsz) return 0;                 /* no __thread data — nothing to set up */
	if (align < 1) align = 1;

	main_tls.image = image;
	main_tls.len   = filesz;
	main_tls.size  = memsz;
	main_tls.align = align;
	libc.tls_cnt   = 1;
	libc.tls_head  = &main_tls;

	/* musl static_init_tls math, verbatim (x86_64 = !TLS_ABOVE_TP). main_tls.offset (used to place
	 * each variable at tp-offset) is computed with the TRUE segment alignment; libc.tls_align is
	 * only bumped to MIN_TLS_ALIGN afterwards, for the block BASE alignment — so the local-exec
	 * %fs:negoffset the linker baked in still matches the block __copy_tls lays out. */
	main_tls.size += (-main_tls.size - (uintptr_t)main_tls.image) & (main_tls.align - 1);
	main_tls.offset = main_tls.size;
	if (main_tls.align < MIN_TLS_ALIGN) main_tls.align = MIN_TLS_ALIGN;
	libc.tls_align = main_tls.align;
	/* Explicit parens for the +/& precedence musl relies on (+ binds tighter than &): round the
	 * total block size up to MIN_TLS_ALIGN. */
	libc.tls_size = (2*sizeof(void *) + sizeof(struct pthread)
		+ main_tls.size + main_tls.align
		+ MIN_TLS_ALIGN-1) & -MIN_TLS_ALIGN;

	unsigned char *mem;
	if (libc.tls_size <= sizeof builtin_tls) {
		mem = (unsigned char *)builtin_tls;
	} else {
		mem = mmap(0, libc.tls_size, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
		if (mem == MAP_FAILED) return 0;
	}

	pthread_t td = __copy_tls(mem);
	/* __copy_tls set td->dtv; finish the minimal TCB init the main thread needs (mirrors the used
	 * parts of musl __init_tp, minus __sysinfo/set_tid_address plumbing we don't have here). errno
	 * (td->errno_val) is already zero from the fresh block. */
	td->self         = td;
	td->detach_state = DT_JOINABLE;
	/* (td->locale intentionally left NULL: picolibc's locale_t differs from musl's, and nothing on
	 * the program's TLS path needs a per-thread locale here.) */
	td->robust_list.head = &td->robust_list.head;
	td->next = td->prev  = td;
	{ long tid; __asm__ __volatile__("syscall" : "=a"(tid) : "a"((long)186) : "rcx","r11","memory");
	  td->tid = (int)tid; }                /* SYS_gettid — x86_64 = 186 (NOT 224, which is i386's
	                                        * gettid). 224 returned -ENOSYS, so the main thread's tid
	                                        * was -38; a non-NORMAL mutex (libuv ERRORCHECK) then saw
	                                        * own=(tid&0x3fffffff) != self->tid and returned EPERM, so
	                                        * uv_mutex_unlock abort()ed. Normal mutexes skip the owner
	                                        * check, which is why the pthread port passed with 224. */

	if (__set_thread_area(td) < 0) return 0;
	return td;
}

/* nx_stream_bridge.c — bridge libc.ndl's stdio-stream DATA exports to plain global symbols.
 *
 * The dllimport shim (nx-dllimport.h) rewrites stdout/stderr/stdin -> (*__imp_<name>) for Mesa's
 * C TUs, but it is deliberately NOT applied to C++ (its _ctype_b macro is unsafe for libstdc++'s
 * <cctype>/<locale>). A handful of Mesa C++ TUs (the GLSL compiler's NIR_DEBUG dumps, glsl_lexer,
 * log_uniform) and libdrm's xf86drm.c therefore reference stdout/stderr/stdin as direct symbols,
 * which a -no-pie .nxe linked against the shared libc.ndl cannot otherwise satisfy.
 *
 * We DEFINE the three globals here so the linker resolves those references, then fill them from
 * the __imp_ IAT slots (which the loader relocated to the real streams inside libc.ndl) via
 * nx_bind_std_streams() at the top of main(). All such uses are debug-only, but binding keeps the
 * streams valid if hit.
 *
 * Deliberately NO <stdio.h>: picolibc declares `stdout` as `FILE *const`, so a writable definition
 * would clash on the const qualifier. The linker matches by symbol name only (qualifiers/types are
 * compile-time), and a FILE* is one pointer wide — so void* storage is byte-correct. */
extern void **__imp_stdin;
extern void **__imp_stdout;
extern void **__imp_stderr;

void *stdin;
void *stdout;
void *stderr;

void nx_bind_std_streams(void)
{
    stdin  = *__imp_stdin;
    stdout = *__imp_stdout;
    stderr = *__imp_stderr;
}

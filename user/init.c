/*
 * init — the first NanOS user program (PID 1), lives at /nanos/core/init.nxe.
 *
 * The kernel hands control here; init brings up userland. For now that means
 * becoming the shell via execve() (PID 1 morphs into nsh in place). This is the
 * seam where mounting extra volumes, loading .nkext drivers, and starting services
 * will go before the exec.
 */
int execve(const char* path, char* const argv[], char* const envp[]);  /* libc glue */
/* `environ` (the kernel-provided environment, TERM=…) comes from nx-dllimport.h, which is
 * force-included for program objects; it maps to libc.ndl's environ via the import slot. */

int main(void) {
	char* argv[] = { "nsh", 0 };
	execve("/disks/main/nanos/bin/nsh.nxe", argv, environ);   /* forward our environment */
	return 127;   /* only reached if exec failed */
}

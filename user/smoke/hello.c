/* Plan 10 Task 5 — libc smoke for the x86_64 ring-3 userland.
 *
 * The minimal in-tree init.nxe is freestanding (no libc); this program instead links the
 * REAL picolibc through libc.ndl (import-by-name), so a successful run proves the full C
 * library + dynamic loader work in ring 3 on x86_64. Built by `make hello` against the
 * injected x86_64-nanos sysroot.
 */
#include <stdio.h>
#include <unistd.h>

int main(void)
{
	printf("hello x86_64-nanos\n");
	fflush(stdout);
	return 0;
}

/*
 * init — the first NanOS user program (PID 1), lives at /nanos/core/init.nxe.
 *
 * The kernel hands control here; init brings up userland. For now that means
 * launching the shell via spawn(). This is the seam where mounting extra volumes,
 * loading .nkext drivers, and starting services will go.
 */
int spawn(const char* path, char* const argv[]);   /* libc glue (int 0x80) */

int main(void) {
	char* argv[] = { "nsh", 0 };
	return spawn("/disks/main/nanos/bin/nsh.nxe", argv);
}

/* user/init64.c — minimal 64-bit PID 1 proof: open + read + write grub.cfg, then exit.
 * Mirrors the historical i686 init that cats grub.cfg through the imported API. No libc —
 * just the libnanos syscall wrappers (the picolibc/nsh path is Plan 8). */
#include "libnanos.h"
int main(int argc, char** argv, char** envp) {
	(void) argc; (void) argv; (void) envp;
	const char* hello = "NanOS x86_64 userland: ring 3 + syscall OK\n";
	unsigned n = 0; while (hello[n]) n++;
	write(1, hello, n);
	int fd = open("/disks/main/boot/grub/grub.cfg", 0);   // O_RDONLY
	if (fd >= 0) {
		char buf[512];
		int r;
		while ((r = read(fd, buf, sizeof buf)) > 0)
			write(1, buf, (unsigned) r);
		close(fd);
	}
	exit(0);
	return 0;
}

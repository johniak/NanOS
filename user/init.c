#include "libnanos.h"

// First NanOS userspace program: cat /boot/grub/grub.cfg through the imported
// API (no raw syscalls), then exit.
int main(void) {
	write(1, "init: cat /boot/grub/grub.cfg\n", 30);
	int fd = open("/boot/grub/grub.cfg", 0);
	if (fd < 0) {
		write(1, "init: open failed\n", 18);
		return 1;
	}
	char buf[128];
	int n;
	while ((n = read(fd, buf, sizeof(buf))) > 0)
		write(1, buf, n);
	close(fd);
	return 0;
}

/*
 * mmaptest — exercise mmap(2) in QEMU: anonymous and file-backed mappings.
 *
 * Verifies the kernel actually maps memory (not just /dev/fb0): an anonymous mapping comes
 * back zeroed and is writable+readable across more than one page, and a file-backed mapping
 * reflects the file's contents. Prints one PASS/FAIL line.
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

/* mmap + flags declared locally (no sys/mman.h on our libc include path), same prototype
 * the terminal uses. The kernel detects anonymous mappings by a negative fd. */
extern void* mmap(void* addr, unsigned long length, int prot, int flags, int fd, long off);
#define PROT_READ      1
#define PROT_WRITE     2
#define MAP_PRIVATE    2
#define MAP_ANONYMOUS  0x20

static int fails = 0;
static void check(const char* what, int ok) {
	if (!ok) { fails++; printf("  FAIL %s\n", what); }
}

int main(void) {
	/* Anonymous: two pages, must arrive zeroed, then be writable + readable. */
	unsigned len = 8192;
	char* a = (char*) mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	check("anon mmap returns a mapping", a != (char*) -1 && a != 0);
	if (a != (char*) -1 && a != 0) {
		int zero = 1;
		for (unsigned i = 0; i < len; i += 512) if (a[i] != 0) zero = 0;
		check("anon mapping is zero-filled", zero);
		for (unsigned i = 0; i < len; i++) a[i] = (char) (i * 7 + 3);
		int ok = 1;
		for (unsigned i = 0; i < len; i++) if (a[i] != (char) (i * 7 + 3)) ok = 0;
		check("anon mapping holds written bytes across 2 pages", ok);
	}

	/* A second anonymous mapping must not alias the first. */
	char* b = (char*) mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	check("second anon mmap distinct", b != (char*) -1 && b != a);
	if (b != (char*) -1) { b[0] = 'Z'; check("second mapping independent", a == (char*) -1 || a[0] != 'Z'); }

	/* File-backed: map a known file and compare against a normal read. */
	const char* path = "/disks/main/boot/grub/grub.cfg";
	int fd = open(path, O_RDONLY);
	check("open grub.cfg", fd >= 0);
	if (fd >= 0) {
		char want[16];
		int n = read(fd, want, sizeof want);
		char* m = (char*) mmap(0, 16, PROT_READ, MAP_PRIVATE, fd, 0);
		check("file-backed mmap returns a mapping", m != (char*) -1);
		if (m != (char*) -1 && n >= 8)
			check("file-backed mapping matches the file's bytes", memcmp(m, want, 8) == 0);
		close(fd);
	}

	printf("mmaptest: %s\n", fails ? "FAIL" : "OK (anon + file-backed mmap work)");
	return fails ? 1 : 0;
}

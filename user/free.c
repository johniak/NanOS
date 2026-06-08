/*
 * free — show memory usage, like Linux's free(1). Reads /proc/meminfo (the same file
 * Linux's free reads) and prints a total/used/free table for physical RAM and the
 * kernel byte heap. Uses raw read() + a tiny parser (no stdio buffering on /proc).
 */
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>

/* Return the unsigned value following "key" in buf (e.g. "MemFree:   120000 kB"), or 0. */
static unsigned field(const char* buf, const char* key) {
	for (const char* p = buf; *p; p++) {
		const char* k = key;
		const char* q = p;
		while (*k && *q == *k) { q++; k++; }
		if (*k == 0) {                         /* matched the key */
			while (*q == ' ' || *q == '\t') q++;
			unsigned v = 0;
			while (*q >= '0' && *q <= '9') { v = v * 10 + (unsigned) (*q - '0'); q++; }
			return v;
		}
	}
	return 0;
}

int main(void) {
	int fd = open("/proc/meminfo", O_RDONLY);
	if (fd < 0) { printf("free: cannot open /proc/meminfo\n"); return 1; }
	char buf[512];
	int n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if (n < 0) n = 0;
	buf[n] = 0;

	unsigned memTotal = field(buf, "MemTotal:");
	unsigned memFree  = field(buf, "MemFree:");
	unsigned khTotal  = field(buf, "KHeapTotal:");
	unsigned khFree   = field(buf, "KHeapFree:");

	printf("           total        used        free\n");
	printf("Mem:  %10u  %10u  %10u\n", memTotal, memTotal - memFree, memFree);
	printf("Heap: %10u  %10u  %10u\n", khTotal, khTotal - khFree, khFree);
	return 0;
}

/*
 * usbstorm — the ROOT-ON-USB concurrent I/O gate (Dell GL-desktop freeze + wallpaper-crash hunt).
 *
 * The Dell failure signature: nwm-gl loads a 1.6 MiB PNG off the USB root WHILE the login storm
 * (getty, greeter, tee loggers) reads and synchronously appends other files on 4 cores — and the
 * process intermittently dies on SIGSEGV mid-decode, log appends vanish (missing [ring3 fault]
 * lines), and the whole box eventually wedges with every USB-backed log silent. QEMU smokes never
 * exercised that MIX: readers + synchronous appenders + a big sequential read, all concurrent,
 * -smp 4. This program is that mix, with order-independent oracles:
 *
 *   READERS  (R):  read REFFILE fully, checksum, compare against the FIRST read. Any mismatch =
 *                  torn/corrupt read path -> "USBSTORM MISMATCH".
 *   WRITERS  (W):  append 512-B records to a private /nanos/logs/storm<i>.log (each ext write
 *                  flushes synchronously to the stick — exactly the gldiag/nwm.txt write profile).
 *   SHARED  (2):   two processes append fixed-size records to ONE shared file; afterwards the
 *                  parent re-reads it and counts each writer's records. A lost/clobbered append
 *                  (the missing-panic-sink-line class: stat-size + write is not atomic) shows as
 *                  count < expected -> "USBSTORM LOST".
 *   CRASHER (1):   deliberately segfaults mid-storm — the console must show the kernel's
 *                  "[nanos: killed faulting process ...]" line (the evidence funnel) and the
 *                  system must keep running.
 *
 * Success line (the smoke greps for it):  "USBSTORM DONE mism=0 lost=0"
 * A wedge = the line never prints (the smoke times out and fails).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

#define REFFILE   "/disks/main/nanos/share/wallpaper.png"
#define SHAREDLOG "/disks/main/nanos/logs/storm-shared.log"
#define NREAD  3
#define NWRITE 2
#define READ_ITERS  6      /* full 1.6 MiB file reads per reader (per-sector USB is slow) */
#define WRITE_ITERS 40     /* 512-B synchronous appends per writer */
#define SHARED_RECS 30     /* records per shared-file appender */
#define RECSZ 64

static unsigned crc32b(const unsigned char *p, unsigned long n, unsigned c)
{
	c = ~c;
	for (unsigned long i = 0; i < n; i++) {
		c ^= p[i];
		for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
	}
	return ~c;
}

/* Read REFFILE fully; return its crc and length via *len (0 length = open/read failure). */
static unsigned read_ref(unsigned char *buf, unsigned long cap, unsigned long *len)
{
	int fd = open(REFFILE, O_RDONLY);
	unsigned long got = 0;
	if (fd < 0) { *len = 0; return 0; }
	for (;;) {
		int r = read(fd, buf + got, cap - got);
		if (r <= 0) break;
		got += (unsigned long) r;
		if (got == cap) break;
	}
	close(fd);
	*len = got;
	return crc32b(buf, got, 0);
}

static int reader(int id)
{
	unsigned long cap = 2u << 20;
	unsigned char *buf = malloc(cap);
	unsigned long len0 = 0, len = 0;
	unsigned ref, c;
	int bad = 0;
	if (!buf) { printf("usbstorm r%d: OOM\n", id); return 1; }
	ref = read_ref(buf, cap, &len0);
	if (!len0) { printf("usbstorm r%d: reference read failed\n", id); return 1; }
	for (int i = 0; i < READ_ITERS; i++) {
		c = read_ref(buf, cap, &len);
		if (len != len0 || c != ref) {
			printf("USBSTORM MISMATCH r%d iter=%d len=%lu/%lu crc=%08x/%08x\n",
			       id, i, len, len0, c, ref);
			bad++;
		}
	}
	free(buf);
	return bad ? 1 : 0;
}

static int writer(int id)
{
	char path[64], rec[512];
	snprintf(path, sizeof path, "/disks/main/nanos/logs/storm%d.log", id);
	memset(rec, 'a' + id, sizeof rec);
	rec[sizeof rec - 1] = '\n';
	int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
	if (fd < 0) { printf("usbstorm w%d: open failed\n", id); return 1; }
	for (int i = 0; i < WRITE_ITERS; i++) {
		if (write(fd, rec, sizeof rec) != (int) sizeof rec) {
			printf("usbstorm w%d: short write iter=%d\n", id, i);
			close(fd);
			return 1;
		}
		fsync(fd);
	}
	close(fd);
	return 0;
}

/* Append SHARED_RECS fixed-size records tagged with our id to the SHARED file. Mimics the
 * panic-sink vs printk-tee append pattern (two writers, one file, O_APPEND). */
static int shared_appender(int id)
{
	char rec[RECSZ];
	memset(rec, '0' + id, RECSZ - 1);
	rec[RECSZ - 1] = '\n';
	int fd = open(SHAREDLOG, O_WRONLY | O_CREAT | O_APPEND, 0666);
	if (fd < 0) { printf("usbstorm s%d: open failed\n", id); return 1; }
	for (int i = 0; i < SHARED_RECS; i++) {
		write(fd, rec, RECSZ);
		fsync(fd);
	}
	close(fd);
	return 0;
}

static int crasher(void)
{
	volatile int *bad = (volatile int *) 0xDEAD0000;
	*bad = 1;                       /* SIGSEGV mid-storm: the kill must be surgical + logged */
	return 0;                       /* never reached */
}

int main(void)
{
	int pids[NREAD + NWRITE + 3], n = 0, lost = 0, mism = 0;

	unlink(SHAREDLOG);
	printf("usbstorm: start (%d readers, %d writers, 2 shared appenders, 1 crasher)\n",
	       NREAD, NWRITE);

	for (int i = 0; i < NREAD; i++) {
		int p = fork();
		if (p == 0) _exit(reader(i));
		pids[n++] = p;
	}
	for (int i = 0; i < NWRITE; i++) {
		int p = fork();
		if (p == 0) _exit(writer(i));
		pids[n++] = p;
	}
	for (int i = 0; i < 2; i++) {
		int p = fork();
		if (p == 0) _exit(shared_appender(i));
		pids[n++] = p;
	}
	{
		int p = fork();
		if (p == 0) _exit(crasher());
		pids[n++] = p;
	}

	for (int i = 0; i < n; i++) {
		int st = 0;
		waitpid(pids[i], &st, 0);
		/* reader exit 1 = mismatch(es) already printed; count for the DONE line */
		if (WIFEXITED(st) && WEXITSTATUS(st) == 1)
			mism++;
	}

	/* Shared-append oracle: every record must have survived (2 * SHARED_RECS * RECSZ bytes,
	 * SHARED_RECS records per id). A lost append = the two appenders raced the size lookup. */
	{
		unsigned long cap = 2u * SHARED_RECS * RECSZ + RECSZ;
		unsigned char *buf = malloc(cap);
		int cnt[2] = { 0, 0 };
		int fd = open(SHAREDLOG, O_RDONLY);
		long got = 0;
		if (fd >= 0 && buf) {
			int r;
			while ((r = read(fd, buf + got, cap - got)) > 0) got += r;
			close(fd);
			for (long o = 0; o + RECSZ <= got; o += RECSZ) {
				if (buf[o] == '0') cnt[0]++;
				else if (buf[o] == '1') cnt[1]++;
			}
		}
		if (cnt[0] != SHARED_RECS) { printf("USBSTORM LOST id=0 got=%d want=%d\n", cnt[0], SHARED_RECS); lost += SHARED_RECS - cnt[0]; }
		if (cnt[1] != SHARED_RECS) { printf("USBSTORM LOST id=1 got=%d want=%d\n", cnt[1], SHARED_RECS); lost += SHARED_RECS - cnt[1]; }
		free(buf);
	}

	/* The crasher must have died by SIGSEGV (signal 11), not run to completion. */
	printf("USBSTORM DONE mism=%d lost=%d\n", mism > 0 ? mism : 0, lost);
	return 0;
}

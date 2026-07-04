/*
 * ptytest — PTY + poll() latency/correctness probes (grown from the Stage-2 round-trip test
 * while chasing a "terminal is one keystroke behind" report; the kernel came out clean, but
 * these probes pin the whole event path and stay as the regression oracle). Each prints
 * PASS/FAIL + latency in ms:
 *   1. poll-wake:  child writes to the slave after ~1 s; parent poll(master) must return ~1 s,
 *                  not at the 8 s timeout (does slave output WAKE a master poller?).
 *   2. scan:       child writes while parent is asleep; parent then poll(master, 0) — POLLIN
 *                  must be set (does a non-blocking scan SEE buffered data?).
 *   3. echo:       parent writes a byte to the master (cooked mode, ECHO on); the line-
 *                  discipline echo must make the master readable immediately.
 *   4. rt:         parent writes a canonical line; child (blocked in slave read) must wake,
 *                  read it, and write a reply; parent polls for the reply (full round trip).
 *   5. sock-wake:  as 1 but for an AF_UNIX socketpair.
 *   6/7. two-fd:   poll({socket, master}) mirrors the terminal app's fd set; the wake must
 *                  land in the RIGHT pollfd slot (ABI stride check) from either side.
 */
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>

static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void msleep(int ms) { poll(0, 0, ms); }

int main(void)
{
	int m = open("/dev/ptmx", O_RDWR);
	int s = open("/dev/pts0", O_RDWR);
	if (m < 0 || s < 0) { printf("ptytest: open failed (m=%d s=%d)\n", m, s); return 1; }
	char buf[256];
	struct pollfd pf;

	/* drain anything pending on the master */
	pf.fd = m; pf.events = POLLIN; pf.revents = 0;
	while (poll(&pf, 1, 0) > 0 && (pf.revents & POLLIN)) { if (read(m, buf, sizeof buf) <= 0) break; pf.revents = 0; }

	/* ---- 1. poll-wake ---- */
	int pid = fork();
	if (pid == 0) { msleep(1000); write(s, "X\n", 2); _exit(0); }
	long t0 = now_ms();
	pf.fd = m; pf.events = POLLIN; pf.revents = 0;
	int r = poll(&pf, 1, 8000);
	long dt = now_ms() - t0;
	printf("ptytest: 1 poll-wake  %s r=%d rev=0x%x dt=%ldms (expect ~1000)\n",
	       (r > 0 && dt < 2500) ? "PASS" : "FAIL", r, pf.revents, dt);
	read(m, buf, sizeof buf);

	/* ---- 2. scan ---- */
	pid = fork();
	if (pid == 0) { msleep(300); write(s, "Y\n", 2); _exit(0); }
	msleep(1500);                         /* data lands while we are NOT polling */
	pf.fd = m; pf.events = POLLIN; pf.revents = 0;
	r = poll(&pf, 1, 0);
	printf("ptytest: 2 scan       %s r=%d rev=0x%x (expect POLLIN set)\n",
	       (r > 0 && (pf.revents & POLLIN)) ? "PASS" : "FAIL", r, pf.revents);
	read(m, buf, sizeof buf);

	/* ---- 3. echo ---- */
	t0 = now_ms();
	write(m, "a", 1);                     /* cooked+ECHO: the kernel echoes 'a' to the master */
	pf.fd = m; pf.events = POLLIN; pf.revents = 0;
	r = poll(&pf, 1, 2000);
	dt = now_ms() - t0;
	printf("ptytest: 3 echo       %s r=%d rev=0x%x dt=%ldms (expect ~0)\n",
	       (r > 0 && dt < 200) ? "PASS" : "FAIL", r, pf.revents, dt);
	read(m, buf, sizeof buf);
	write(m, "\n", 1); read(s, buf, sizeof buf);   /* flush the canonical line to the slave */

	/* ---- 4. rt (slave-read wake + reply) ---- */
	pid = fork();
	if (pid == 0) {
		int n = read(s, buf, sizeof buf);   /* blocks until the parent's line arrives */
		if (n > 0) write(s, "R\n", 2);
		_exit(0);
	}
	msleep(300);                            /* let the child block in read() first */
	t0 = now_ms();
	write(m, "z\n", 2);
	/* eat the echo of "z\n" first, then wait for the child's "R\n" reply */
	long deadline = t0 + 8000;
	int got_r = 0;
	while (now_ms() < deadline) {
		pf.fd = m; pf.events = POLLIN; pf.revents = 0;
		if (poll(&pf, 1, (int)(deadline - now_ms())) <= 0) break;
		int n = read(m, buf, sizeof buf);
		for (int i = 0; i < n; i++) if (buf[i] == 'R') got_r = 1;
		if (got_r) break;
	}
	dt = now_ms() - t0;
	printf("ptytest: 4 rt         %s dt=%ldms (expect <200)\n",
	       (got_r && dt < 1000) ? "PASS" : "FAIL", dt);

	/* ---- 5. sock-wake: does a unix-socket write WAKE a poller? ---- */
	int sv[2];
	if (socketpair(1 /*AF_UNIX*/, 1 /*SOCK_STREAM*/, 0, sv) != 0) {
		printf("ptytest: 5/6/7 SKIP (socketpair failed)\n");
	} else {
		pid = fork();
		if (pid == 0) { msleep(1000); write(sv[1], "S", 1); _exit(0); }
		t0 = now_ms();
		pf.fd = sv[0]; pf.events = POLLIN; pf.revents = 0;
		r = poll(&pf, 1, 8000);
		dt = now_ms() - t0;
		printf("ptytest: 5 sock-wake  %s r=%d rev=0x%x dt=%ldms (expect ~1000)\n",
		       (r > 0 && dt < 2500) ? "PASS" : "FAIL", r, pf.revents, dt);
		read(sv[0], buf, sizeof buf);

		/* ---- 6. two-fd mirror of terminal.c: poll({sock, master}) — pty data must wake and
		 * must be reported in pf[1], not smeared into pf[0] (ABI stride check). ---- */
		pid = fork();
		if (pid == 0) { msleep(1000); write(s, "M\n", 2); _exit(0); }
		struct pollfd p2[2];
		p2[0].fd = sv[0]; p2[0].events = POLLIN; p2[0].revents = 0;
		p2[1].fd = m;     p2[1].events = POLLIN; p2[1].revents = 0;
		t0 = now_ms();
		r = poll(p2, 2, 8000);
		dt = now_ms() - t0;
		printf("ptytest: 6 two-fd-pty %s r=%d rev0=0x%x rev1=0x%x dt=%ldms (expect rev1=1, ~1000)\n",
		       (r > 0 && (p2[1].revents & POLLIN) && !p2[0].revents && dt < 2500) ? "PASS" : "FAIL",
		       r, p2[0].revents, p2[1].revents, dt);
		read(m, buf, sizeof buf);

		/* ---- 7. two-fd, socket side: the same set, woken by the SOCKET instead. ---- */
		pid = fork();
		if (pid == 0) { msleep(1000); write(sv[1], "T", 1); _exit(0); }
		p2[0].fd = sv[0]; p2[0].events = POLLIN; p2[0].revents = 0;
		p2[1].fd = m;     p2[1].events = POLLIN; p2[1].revents = 0;
		t0 = now_ms();
		r = poll(p2, 2, 8000);
		dt = now_ms() - t0;
		printf("ptytest: 7 two-fd-sck %s r=%d rev0=0x%x rev1=0x%x dt=%ldms (expect rev0=1, ~1000)\n",
		       (r > 0 && (p2[0].revents & POLLIN) && !p2[1].revents && dt < 2500) ? "PASS" : "FAIL",
		       r, p2[0].revents, p2[1].revents, dt);
		read(sv[0], buf, sizeof buf);
	}

	printf("ptytest: done\n");
	fflush(stdout);
	return 0;
}

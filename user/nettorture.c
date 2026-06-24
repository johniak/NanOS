/*
 * nettorture — the SMP NETWORK data-race gate for retiring the Big Kernel Lock (Phase 4, Task 15e).
 *
 * After the BKL is gone, the net RX-softirq thread (ksoftirqd-net) and the net-timer thread run
 * CONCURRENTLY with socket syscalls; without g_netLock they would corrupt the shared socket table
 * (g_socks) and UDP delivery state. This gate makes them collide: NT threads each bind a UDP socket
 * on loopback and ping-pong datagrams to THEMSELVES — every sendto() crosses ipOutput -> loopback
 * -> netifRx -> the RX-softirq thread -> udpRx -> socketDeliver -> the socket's rxq, while the
 * recvfrom() (blocked on rxWait) is woken by that same softirq thread. So three contexts hit the
 * net stack at once on >1 core. Each datagram carries (id,seq); the receiver verifies it byte-for-
 * byte. A race that misdelivers to the wrong socket, corrupts a datagram, or loses a wakeup breaks
 * the per-thread sequence -> the final success line never prints. Deterministic: a thread waits for
 * its own datagram back before sending the next, so a correct stack never drops one.
 *
 * Success line (the gate proof):
 *     nettorture: 6 threads x 300 datagrams ok
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define NT    6        /* threads (each its own UDP socket on a distinct loopback port) */
#define ITER  300      /* ping-pong rounds per thread */
#define BASEP 41000    /* base UDP port */

static volatile int net_fail = 0;

static void *udp_worker(void *a)
{
	long id = (long)a;
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) { net_fail = 1; return 0; }

	struct sockaddr_in me;
	memset(&me, 0, sizeof me);
	me.sin_family = AF_INET;
	me.sin_port = htons((unsigned short)(BASEP + id));
	me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* 127.0.0.1 */
	if (bind(fd, (struct sockaddr *)&me, sizeof me) < 0) { net_fail = 1; close(fd); return 0; }

	for (int i = 0; i < ITER; i++) {
		char msg[24];
		/* a pattern encoding (id,i): a misdelivery to another thread's socket, or a corrupt
		 * datagram, fails the exact compare below. */
		int n = 0;
		msg[n++] = (char)('A' + (int)id);
		msg[n++] = (char)(i & 0xff);
		msg[n++] = (char)((i >> 8) & 0xff);
		for (int k = 3; k < 20; k++) msg[k] = (char)(msg[0] + k + i);
		int mlen = 20;

		if (sendto(fd, msg, mlen, 0, (struct sockaddr *)&me, sizeof me) != mlen) {
			net_fail = 1; close(fd); return 0;
		}
		char rb[64];
		struct sockaddr_in from;
		socklen_t fl = sizeof from;
		int r = recvfrom(fd, rb, sizeof rb, 0, (struct sockaddr *)&from, &fl);
		if (r != mlen || memcmp(rb, msg, mlen) != 0) { net_fail = 1; close(fd); return 0; }
	}
	close(fd);
	return 0;
}

int main(void)
{
	printf("nettorture: start\n"); fflush(stdout);
	pthread_t t[NT];
	for (long i = 0; i < NT; i++)
		if (pthread_create(&t[i], 0, udp_worker, (void *)i) != 0) {
			printf("nettorture: thread create FAIL\n"); return 1;
		}
	for (int i = 0; i < NT; i++) pthread_join(t[i], 0);
	if (net_fail) { printf("nettorture: NET RACE — datagram mismatch/loss/misdelivery\n"); return 1; }
	printf("nettorture: %d threads x %d datagrams ok\n", NT, ITER);
	fflush(stdout);
	return 0;
}

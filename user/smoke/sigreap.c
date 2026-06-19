/*
 * sigreap — isolate the dropbear "zombie children never reaped" behaviour on NanOS.
 *
 * Reproduces dropbear's EXACT reaping pattern (svr-main.c): a SIGCHLD handler installed with
 * SA_NOCLDSTOP (NO SA_RESTART) that drains children with `while (waitpid(-1,NULL,WNOHANG) > 0)`,
 * while the parent blocks in select() on a pipe (like dropbear's accept loop). We fork N children
 * that _exit immediately, then spin the select loop; if the handler fires and reaps, `g_reaped`
 * reaches N. Runs as init (PID 1) for a clean, dropbear-free measurement; output goes to the
 * console -> COM1 serial.
 */
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <sys/select.h>

int waitpid(int pid, int* status, int options);   /* as in forkmany.c / nsh: not in the glue headers */

#define N 10
static volatile int g_reaped = 0;
static volatile int g_entered = 0;   /* handler invocations, regardless of what waitpid returns */

static void sigchld_handler(int sig) {
	(void) sig;
	g_entered++;
	int saved = 0;
	while (waitpid(-1, &saved, 1 /*WNOHANG*/) > 0)   /* drain, exactly like dropbear */
		g_reaped++;
	/* dropbear re-installs itself inside the handler */
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigchld_handler;
	sa.sa_flags = SA_NOCLDSTOP;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGCHLD, &sa, NULL);
}

int main(void) {
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigchld_handler;
	sa.sa_flags = SA_NOCLDSTOP;          /* deliberately NO SA_RESTART, like dropbear */
	sigemptyset(&sa.sa_mask);
	int sar = sigaction(SIGCHLD, &sa, NULL);
	printf("SIGREAP: sigaction ret=%d\n", sar); fflush(stdout);

	/* Isolate handler INSTALL+DELIVER from child-exit posting: raise SIGCHLD to ourselves, then
	 * make a syscall (the delivery point). If g_entered becomes 1, install+deliver work. */
	raise(SIGCHLD);
	(void) getpid();
	printf("SIGREAP: after raise(SIGCHLD): g_entered=%d\n", g_entered); fflush(stdout);
	g_entered = 0;

	int pfd[2];
	if (pipe(pfd) < 0) { printf("SIGREAP: pipe failed\n"); fflush(stdout); return 1; }

	printf("SIGREAP: mypid=%d\n", (int) getpid()); fflush(stdout);
	int spawned = 0;
	for (int i = 0; i < N; i++) {
		int pid = fork();
		if (pid == 0) { _exit(0); }       /* child: exit immediately -> SIGCHLD to parent */
		if (pid > 0) { spawned++; printf("SIGREAP: forked child pid=%d\n", pid); fflush(stdout); }
	}
	printf("SIGREAP: spawned=%d, entering select loop\n", spawned); fflush(stdout);

	/* dropbear-style: block in select on the pipe read-end with a timeout; the SIGCHLD handler
	 * must interrupt it (EINTR, since no SA_RESTART) and reap. */
	int directReaped = 0;
	for (int iter = 0; iter < 20 && (g_reaped + directReaped) < spawned; iter++) {
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(pfd[0], &rfds);
		struct timeval tv = { 0, 200 * 1000 };   /* 200 ms */
		int r = select(pfd[0] + 1, &rfds, NULL, NULL, &tv);
		/* H_B probe: a DIRECT WNOHANG reap in the loop body. If this reaps children that the
		 * handler did not, the children ARE ours+exited and the failure is signal-during-select. */
		int st, d = 0;
		while (waitpid(-1, &st, 1 /*WNOHANG*/) > 0) { d++; directReaped++; }
		printf("SIGREAP: iter=%d select=%d handlerEntered=%d handlerReaped=%d directThisIter=%d\n",
		       iter, r, g_entered, g_reaped, d); fflush(stdout);
	}

	printf("SIGREAP: RESULT handlerReaped=%d directReaped=%d / spawned=%d  %s\n",
	       g_reaped, directReaped, spawned,
	       (g_reaped == spawned) ? "OK-HANDLER-REAPS"
	       : (g_reaped + directReaped == spawned) ? "HANDLER-DEAD-BUT-DIRECT-WAITPID-OK (signal-during-select broken)"
	       : "FAIL-CHILDREN-NOT-REAPABLE");
	fflush(stdout);
	for (;;) pause();                     /* init must not return */
	return 0;
}

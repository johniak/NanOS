/*
 * init — the first NanOS user program (PID 1), lives at /nanos/core/init.nxe.
 *
 * The kernel hands control here; init brings up userland. For now that means becoming the
 * login shell via execve() (PID 1 morphs into the shell in place). The shell is NOT
 * hardcoded: like a Unix login, init reads the login shell from the account database
 * (getpwuid -> pw_shell, the 7th field of the passwd file — NanOS keeps it under
 * /nanos/config, not /etc) and exports it as $SHELL. Change that file to change the default
 * shell. Falls back to nsh if that shell is absent. This is also the seam where mounting
 * extra volumes / starting services will go.
 */
#include <unistd.h>
#include <pwd.h>
#include <string.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>

int execve(const char* path, char* const argv[], char* const envp[]);
/* `environ` (the kernel-provided environment, TERM=…) comes from nx-dllimport.h, which is
 * force-included for program objects; it maps to libc.ndl's environ via the import slot. */

#define FALLBACK_SHELL "/disks/main/nanos/bin/nsh.nxe"
#define UDHCPC "/disks/main/nanos/bin/udhcpc.nxe"
#define INETD  "/disks/main/nanos/bin/inetd.nxe"
#define HTTPD  "/disks/main/nanos/bin/darkhttpd.nxe"
#define INETD_CONF "/disks/main/nanos/config/etc/inetd.conf"
#define WWWROOT    "/disks/main/apps/www"

/* Configure networking via DHCP before starting the shell: run the real busybox udhcpc, which
 * acquires a lease over AF_PACKET and exec()s /nanos/config/udhcpc.script to set the address,
 * route and resolv.conf. -f -q -t 4 -n keeps it in the foreground and bounded — it exits after a
 * lease (or 4 failed tries), so init never hangs. If udhcpc is absent, the kernel's static
 * configuration stays in place as the (loud) fallback. */
static void run_dhcp(void) {
	if (access(UDHCPC, F_OK) != 0)
		return;
	int pid = fork();
	if (pid == 0) {
		char* a[] = { (char*) "udhcpc", (char*) "-i", (char*) "eth0",
		              (char*) "-f", (char*) "-q", (char*) "-t", (char*) "4", (char*) "-n", 0 };
		execve(UDHCPC, a, environ);
		_exit(127);
	}
	if (pid <= 0)
		return;
	/* Bound the lease attempt to ~10s: poll for udhcpc to finish, then kill it and fall back to
	 * the kernel's static config — boot must never hang on DHCP (the plan's loud fallback). */
	for (int i = 0; i < 100; i++) {
		int st;
		if (waitpid(pid, &st, WNOHANG) == pid)
			return;                                  // udhcpc got a lease (or gave up) — done
		struct timespec ts = { 0, 100 * 1000 * 1000 };   // 100 ms
		nanosleep(&ts, 0);
	}
	kill(pid, SIGKILL);
	waitpid(pid, 0, 0);
}

/* Start a self-daemonizing network service: fork, the child execs it, the parent reaps the
 * short-lived launcher process (the service calls daemon()/--daemon, so the process we forked
 * exits once the real daemon — reparented to us, PID 1 — is running). Skipped silently if the
 * binary is absent (an image built without the optional services still boots). A one-line
 * console note records which services came up, like the DHCP path (no silent magic). */
static void start_service(const char* path, char* const argv[]) {
	if (access(path, X_OK) != 0)
		return;
	int pid = fork();
	if (pid == 0) {
		execve(path, argv, environ);
		_exit(127);
	}
	if (pid > 0)
		waitpid(pid, 0, 0);            // reap the launcher; daemon() already backgrounded the server
}

/* Bring up the listening services after the network is configured: just inetd (the super-server:
 * echo/daytime/... + telnet -> telnetd login). It daemonizes itself; its grandchildren reparent
 * to init. darkhttpd is NOT started by default — start it by hand when wanted:
 *   darkhttpd /disks/main/apps/www --port 80 --daemon
 * (its binary still ships in /nanos/bin; only the boot-time autostart is gone). */
static void start_services(void) {
	char* inetd_argv[] = { (char*) "inetd", (char*) "--pidfile=/tmp/inetd.pid",
	                       (char*) INETD_CONF, 0 };
	start_service(INETD, inetd_argv);
}

/* argv[0] for a shell at `path`: its basename with any ".nxe" suffix stripped, so the shell
 * presents itself as "bash"/"nsh", not "bash.nxe". Written into `out`. */
static const char* shell_argv0(const char* path, char* out, int cap) {
	const char* base = strrchr(path, '/');
	base = base ? base + 1 : path;
	int i = 0;
	while (base[i] && i < cap - 1) { out[i] = base[i]; i++; }
	out[i] = 0;
	if (i >= 4 && strcmp(out + i - 4, ".nxe") == 0)
		out[i - 4] = 0;
	return out;
}

int main(void) {
	run_dhcp();        /* bring up eth0 via DHCP before the shell (kernel static = fallback) */
	start_services();  /* start the listening services (inetd + httpd) once the network is up */
	struct passwd* pw = getpwuid(getuid());
	const char* shell = (pw && pw->pw_shell && pw->pw_shell[0]) ? pw->pw_shell : FALLBACK_SHELL;

	static char name0[64];
	shell_argv0(shell, name0, sizeof name0);

	/* Hand the login shell a Unix-like environment, as login(1) does: $SHELL (the login
	 * shell), $HOME (from the passwd entry — without it `cd` with no args fails "HOME not
	 * set"), and a default $PATH so it can run the system utilities by name. TERM/TERMINFO
	 * already come from the kernel via `environ`. */
	static char shellvar[160];
	strcpy(shellvar, "SHELL=");
	strncat(shellvar, shell, sizeof shellvar - 7);

	static char homevar[160];
	strcpy(homevar, "HOME=");
	strncat(homevar, (pw && pw->pw_dir && pw->pw_dir[0]) ? pw->pw_dir : "/", sizeof homevar - 6);

	char* newenv[64];
	int n = 0;
	for (char** e = environ; *e && n < 59; e++)
		newenv[n++] = *e;
	newenv[n++] = shellvar;
	newenv[n++] = homevar;
	newenv[n++] = (char*) "PATH=/disks/main/nanos/bin:/disks/main/bin";
	newenv[n] = 0;

	char* argv[] = { name0, 0 };
	execve(shell, argv, newenv);

	/* The configured shell failed to load (e.g. an image without the optional bash) — fall
	 * back to nsh so the system is never left without a shell. */
	char* fbargv[] = { (char*) "nsh", 0 };
	execve(FALLBACK_SHELL, fbargv, newenv);
	return 127;   /* only reached if even nsh failed */
}

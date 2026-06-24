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
#include <fcntl.h>
#include <sys/ioctl.h>
#ifndef TIOCSCTTY
#define TIOCSCTTY 0x540E
#endif

/* Text virtual consoles getty'd at boot: /dev/tty1../dev/tty6 (Ctrl+Alt+F1..F6). tty7 is the
 * graphics VT (nwm) launched separately. */
#define NVT 6
static int g_vtpid[NVT + 1];   /* the login pid running on each text VT (for getty-style respawn) */

int execve(const char* path, char* const argv[], char* const envp[]);
/* `environ` (the kernel-provided environment, TERM=…) comes from nx-dllimport.h, which is
 * force-included for program objects; it maps to libc.ndl's environ via the import slot. */

#define FALLBACK_SHELL "/disks/main/nanos/bin/nsh.nxe"
#define UDHCPC "/disks/main/nanos/bin/udhcpc.nxe"
#define INETD  "/disks/main/nanos/bin/inetd.nxe"
#define HTTPD  "/disks/main/nanos/bin/darkhttpd.nxe"
#define INETD_CONF "/disks/main/nanos/config/etc/inetd.conf"
#define DROPBEAR    "/disks/main/nanos/bin/dropbear.nxe"
#define DROPBEARKEY "/disks/main/nanos/bin/dropbearkey.nxe"
#define SSH_HOSTKEY "/disks/main/nanos/config/dropbear_ed25519_host_key"
#define WWWROOT    "/disks/main/apps/www"

/* Boot/service log. init's own notes AND every service's stdout/stderr are written here, NOT to
 * the interactive console — so the shell the user lands in stays clean. `cat /tmp/boot.log` reads
 * it. g_logfd is opened in main(); until then (and if the open fails) it falls back to fd 1. */
static int g_logfd = 1;
static void note(const char* s) { write(g_logfd, s, (int) strlen(s)); }
/* In a freshly forked service child: redirect its stdout+stderr to the log (the daemon's chatter
 * — udhcpc leases, dropbear connection logs — lands in the log, not the console). */
static void log_redirect_child(void) { if (g_logfd > 2) { dup2(g_logfd, 1); dup2(g_logfd, 2); } }

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
		log_redirect_child();
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
	if (access(path, X_OK) != 0) {
		note("  [init] skip "); note(argv[0] ? argv[0] : path); note(" (not installed)\n");
		return;
	}
	note("  [init] starting "); note(argv[0] ? argv[0] : path); note("\n");
	int pid = fork();
	if (pid == 0) {
		log_redirect_child();
		execve(path, argv, environ);
		_exit(127);
	}
	if (pid > 0)
		waitpid(pid, 0, 0);            // reap the launcher; daemon() already backgrounded the server
}

/* Start the SSH server (Dropbear) as a boot service. On FIRST boot it generates a persistent
 * ed25519 host key on the read-write disk, so the key is stable across reboots (no client
 * host-key-changed warnings); later boots reuse it. Login is by password (the hash in
 * /nanos/config/passwd) or ~/.ssh/authorized_keys. From the host (the `make run` hostfwd maps
 * 2222->22): `ssh -p 2222 root@localhost`. Skipped if the binary is absent. */
static void start_sshd(void) {
	if (access(DROPBEAR, X_OK) != 0)
		return;
	if (access(SSH_HOSTKEY, F_OK) != 0) {        // first boot: make the host key (ed25519 is fast)
		note("  [init] generating SSH host key...\n");
		int pid = fork();
		if (pid == 0) {
			char* a[] = { (char*) "dropbearkey", (char*) "-t", (char*) "ed25519",
			              (char*) "-f", (char*) SSH_HOSTKEY, 0 };
			execve(DROPBEARKEY, a, environ);
			_exit(127);
		}
		if (pid > 0)
			waitpid(pid, 0, 0);
	}
	/* Run dropbear in the FOREGROUND (-F) but background it OURSELVES with a plain fork: the child
	 * exec's `dropbear -F`, init (the parent) does NOT waitpid on it and returns to run the local
	 * console shell. init's main reap loop later collects it (and any session grandchildren that
	 * reparent to PID 1).
	 *
	 * Why not let dropbear daemonize (its own daemon() double-fork)? On x86_64 the per-connection
	 * session child faults in ring 3 (vec=0e err=0x15 rip=0x0 — a call through a null pointer) right
	 * after KEX when dropbear has detached via daemon(): the double-fork + setsid + serving the
	 * listening socket inherited across the detach leaves dropbear's fork->exec-shell session path
	 * broken. In -F the same process that created the listening socket is the one that accept()s and
	 * forks each session, and that path is verified to serve full host->guest SSH logins into bash on
	 * x86_64. Backgrounding -F at the init level keeps that working session path AND frees PID 1 to
	 * run the console. Unified across arches: i686's daemon() path worked, but -F-backgrounded is at
	 * least as correct there. -E sends dropbear's log to the boot log (child stderr is redirected). */
	note("  [init] starting dropbear (foreground, backgrounded by init)\n");
	int pid = fork();
	if (pid == 0) {
		log_redirect_child();
		char* a[] = { (char*) "dropbear", (char*) "-F", (char*) "-E", (char*) "-r",
		              (char*) SSH_HOSTKEY, (char*) "-p", (char*) "22", 0 };
		execve(DROPBEAR, a, environ);
		_exit(127);
	}
	/* parent: deliberately no waitpid — dropbear -F keeps running in the background. */
}

/* Bring up the listening services after the network is configured: inetd (the super-server:
 * echo/daytime/... + telnet -> telnetd login), which daemonizes itself, and sshd (Dropbear),
 * which init backgrounds in foreground mode (see start_sshd). darkhttpd is NOT started by
 * default — start it by hand:
 *   darkhttpd /disks/main/apps/www --port 80 --daemon
 * (its binary still ships in /nanos/bin; only the boot-time autostart is gone). */
static void start_services(void) {
	note("[init] bringing up services\n");
	char* inetd_argv[] = { (char*) "inetd", (char*) "--pidfile=/tmp/inetd.pid",
	                       (char*) INETD_CONF, 0 };
	start_service(INETD, inetd_argv);
	start_sshd();
	note("[init] services up (telnet :23, ssh :22 if installed)\n");
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

/* Spawn a getty/login on /dev/ttyN: new session, open the VT, make it the controlling terminal,
 * wire it to stdin/out/err + the foreground group, then exec `login` (which prompts and execs the
 * user's shell). Falls back to the configured shell, then nsh, if login is absent. Returns the
 * child pid so the parent can respawn this console when its login exits. */
static int spawn_getty(int n, char* const* env, const char* shell, char* name0)
{
	int pid = fork();
	if (pid == 0) {
		setsid();
		char dev[10] = "/dev/tty0";
		dev[8] = (char) ('0' + n);             /* -> /dev/ttyN */
		int fd = open(dev, O_RDWR);
		if (fd < 0)
			_exit(127);
		ioctl(fd, TIOCSCTTY, 0);
		dup2(fd, 0);
		dup2(fd, 1);
		dup2(fd, 2);
		if (fd > 2)
			close(fd);
		tcsetpgrp(0, getpid());                /* this getty's group is the VT's foreground group */
		/* Reset the job-control signals to their defaults (init ignores SIGTTIN/SIGTTOU for itself;
		 * a login session must have default dispositions so the shell's job control works). */
		signal(SIGTTOU, SIG_DFL);
		signal(SIGTTIN, SIG_DFL);
		signal(SIGTSTP, SIG_DFL);
		char* largv[] = { (char*) "login", 0 };
		execve("/disks/main/nanos/bin/login.nxe", largv, env);
		char* sargv[] = { name0, 0 };          /* login absent -> the configured shell directly */
		execve(shell, sargv, env);
		char* fbargv[] = { (char*) "nsh", 0 };  /* ... then nsh, so a VT is never left shell-less */
		execve(FALLBACK_SHELL, fbargv, env);
		_exit(127);
	}
	return pid;
}

int main(void) {
	/* Open the boot/service log on the writable tmpfs and send init's notes + every daemon's
	 * stdout/stderr there instead of the console, so the shell the user lands in is clean
	 * (`cat /tmp/boot.log` to read it). Falls back to fd 1 if the open fails. */
	g_logfd = open("/tmp/boot.log", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (g_logfd < 0)
		g_logfd = 1;

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

	static char* newenv[64];
	int n = 0;
	for (char** e = environ; *e && n < 59; e++)
		newenv[n++] = *e;
	newenv[n++] = shellvar;
	newenv[n++] = homevar;
	newenv[n++] = (char*) "PATH=/disks/main/nanos/bin:/disks/main/bin";
	newenv[n] = 0;

	/* init no longer morphs into the shell with execve(): it has backgrounded `dropbear -F`
	 * above, so it must stay alive as PID 1 to (a) keep an interactive console login shell
	 * running on the keyboard/tty and (b) reap EVERY child — the console shell, the backgrounded
	 * dropbear, and any orphaned grandchildren that reparent to PID 1 — so no zombies accumulate.
	 *
	 * The console is a job-control tty: a freshly forked shell starts in init's process group and
	 * then claims the terminal via tcsetpgrp. After a previous shell exits, the terminal's
	 * foreground group is left pointing at the dead shell, so before each (re)spawn init resets it
	 * to its own group — otherwise the new shell, still in init's group, would SIGTTIN-stop itself
	 * on its first console read. (SIGTTOU/SIGTTIN are ignored here so init never stops on tty I/O.) */
	signal(SIGTTOU, SIG_IGN);
	signal(SIGTTIN, SIG_IGN);

	/* Bring up a login on every text VT (Linux getty-on-tty1..6). Each getty runs in its own
	 * session with /dev/ttyN as its controlling terminal, so Ctrl+Alt+Fn switches between fully
	 * independent login sessions. The active VT at boot is tty1, where the user lands. */
	for (int i = 1; i <= NVT; i++)
		g_vtpid[i] = spawn_getty(i, newenv, shell, name0);

	/* Reaper: collect any child. If it was a console's login, respawn that console (getty-style,
	 * with a short backoff so a crash-looping login can't spin). Other reaped pids (dropbear, an
	 * orphaned grandchild) are just collected. */
	for (;;) {
		int w = waitpid(-1, 0, 0);
		if (w < 0) {                          // nothing to reap right now — back off briefly
			struct timespec ts = { 0, 200 * 1000 * 1000 };
			nanosleep(&ts, 0);
			continue;
		}
		for (int i = 1; i <= NVT; i++) {
			if (w == g_vtpid[i]) {
				struct timespec ts = { 0, 200 * 1000 * 1000 };   // backoff vs a crash-looping login
				nanosleep(&ts, 0);
				g_vtpid[i] = spawn_getty(i, newenv, shell, name0);
				break;
			}
		}
	}
}

/*
 * nanologin — the login program telnetd (FAZA H3) execs after it sets up the pty. NanOS is
 * single-user root with no password, so this is not an authenticator: it just establishes a
 * Unix login environment (the job login(1) does) and execs the account's shell, exactly like
 * init does for the console. telnetd is configured with
 *     --login=/disks/main/nanos/bin/nanologin.nxe
 * (no %-template flags), so we are exec'd with argv[0] only and inherit telnetd's stdio = the
 * pty slave (telnetd already did setsid + dup2 via login_tty in the child).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pwd.h>

#define FALLBACK_SHELL "/disks/main/nanos/bin/nsh.nxe"

extern char** environ;

/* argv[0] for the shell: basename of `path` with a trailing ".nxe" stripped (so it shows up as
 * "bash"/"nsh", not "bash.nxe"). */
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

static int has_env(const char* key) {
	size_t kl = strlen(key);
	for (char** e = environ; *e; e++)
		if (strncmp(*e, key, kl) == 0 && (*e)[kl] == '=')
			return 1;
	return 0;
}

int main(void) {
	struct passwd* pw = getpwuid(getuid());
	const char* shell = (pw && pw->pw_shell && pw->pw_shell[0]) ? pw->pw_shell : FALLBACK_SHELL;
	const char* home  = (pw && pw->pw_dir   && pw->pw_dir[0])   ? pw->pw_dir   : "/";
	const char* user  = (pw && pw->pw_name  && pw->pw_name[0])  ? pw->pw_name  : "root";

	static char name0[64];
	shell_argv0(shell, name0, sizeof name0);

	/* A Unix login environment: $SHELL/$HOME/$USER/$LOGNAME + a default $PATH so the shell can
	 * run the system utilities by name. TERM is supplied by telnetd from the telnet negotiation;
	 * default it to a sane value if the client never sent one. We keep whatever telnetd already
	 * put in `environ` and append/override the login basics. */
	static char shellvar[160], homevar[160], uservar[96], logvar[96];
	strcpy(shellvar, "SHELL=");  strncat(shellvar, shell, sizeof shellvar - 7);
	strcpy(homevar,  "HOME=");   strncat(homevar,  home,  sizeof homevar  - 6);
	strcpy(uservar,  "USER=");   strncat(uservar,  user,  sizeof uservar  - 6);
	strcpy(logvar,   "LOGNAME="); strncat(logvar,  user,  sizeof logvar   - 9);

	char* newenv[64];
	int n = 0;
	for (char** e = environ; *e && n < 56; e++)
		newenv[n++] = *e;
	newenv[n++] = shellvar;
	newenv[n++] = homevar;
	newenv[n++] = uservar;
	newenv[n++] = logvar;
	newenv[n++] = (char*) "PATH=/disks/main/nanos/bin:/disks/main/bin";
	if (!has_env("TERM"))
		newenv[n++] = (char*) "TERM=vt100";
	newenv[n] = 0;

	chdir(home);

	char* argv[] = { name0, 0 };
	execve(shell, argv, newenv);

	/* Configured shell failed to load — fall back to nsh so a telnet login is never shell-less. */
	char* fbargv[] = { (char*) "nsh", 0 };
	execve(FALLBACK_SHELL, fbargv, newenv);
	perror("nanologin: exec shell");
	return 127;
}

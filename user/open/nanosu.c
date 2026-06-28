/*
 * nanosu.c — the privileged half of NanOS's "authenticate to open" (macOS-authorization style).
 *
 * Installed setuid-root. The compositor execs it (as the logged-in user) to launch a program with
 * elevated rights after the user typed root's password into the GUI auth dialog:
 *
 *     NW_AUTH_PASS=<password> nanosu <abs-path> [arg]
 *
 * Because it is setuid-root, nanosu starts with euid 0 — enough to read /etc/shadow and verify the
 * password (same check as login/login). On success it becomes fully root (setgid/setuid 0),
 * scrubs the password from the environment, and execs the target so it runs as root (e.g. an
 * editor that can then read a root-owned file). On a wrong password it just exits — nothing runs.
 *
 * The password travels via the environment, not argv, so it is not visible in the process list;
 * nanosu unsets it before exec so the launched app never sees it.
 */
#include <pwd.h>
#include <shadow.h>
#include <crypt.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <grp.h>

/* Verify `pass` against a user's stored hash (shadow first, then passwd). Mirrors login/login. */
static int check_password(struct passwd *pw, const char *pass)
{
	const char *hash = pw->pw_passwd;
	struct spwd *sp = getspnam(pw->pw_name);
	if (sp && sp->sp_pwdp && sp->sp_pwdp[0]) hash = sp->sp_pwdp;
	if (!hash || !hash[0]) return 1;                 /* no password set -> accept */
	if (hash[0] == 'x' && !hash[1]) return 0;        /* locked */
	char *got = crypt(pass, hash);
	return got && strcmp(got, hash) == 0;
}

/* True if `user` is a member of group `grp` (admins are the "wheel" group, which sudoers grants). */
static int user_in_group(const char *user, const char *grp)
{
	struct group *g = getgrnam(grp);
	if (!g || !g->gr_mem) return 0;
	for (char **m = g->gr_mem; *m; m++)
		if (!strcmp(*m, user)) return 1;
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2 || !argv[1] || !argv[1][0]) {
		fprintf(stderr, "usage: NW_AUTH_PASS=... nanosu <abs-path> [arg]\n");
		return 2;
	}
	const char *pass = getenv("NW_AUTH_PASS");
	if (!pass) pass = "";

	/* Authorize the macOS / sudo way: the invoking user (our REAL uid — we are setuid-root, so
	 * euid is already 0 and can read /etc/shadow) proves it with THEIR OWN password, provided they
	 * are an administrator (a member of the "wheel" group, which this system's sudoers grants ALL).
	 * That is what the auth dialog means by "an administrator password" — your own, as on macOS.
	 * root's password is also accepted (covers a root caller and the classic behaviour). */
	char user[64] = "";
	struct passwd *who = getpwuid(getuid());
	if (who && who->pw_name) { strncpy(user, who->pw_name, sizeof user - 1); user[sizeof user - 1] = 0; }

	int ok = 0;
	if (user[0] && user_in_group(user, "wheel")) {
		struct passwd *up = getpwnam(user);
		if (up && check_password(up, pass)) ok = 1;
	}
	if (!ok) {
		struct passwd *rp = getpwnam("root");
		if (rp && check_password(rp, pass)) ok = 1;
	}
	if (!ok) {
		fprintf(stderr, "nanosu: authentication failed\n");
		return 1;
	}

	/* Authenticated. Become fully root, scrub the secret, then run the target as root. */
	unsetenv("NW_AUTH_PASS");
	setgid(0);
	setuid(0);

	char *av[3];
	av[0] = argv[1];
	av[1] = (argc > 2 && argv[2] && argv[2][0]) ? argv[2] : 0;
	av[2] = 0;
	execv(argv[1], av);            /* keep the current environ (NW_DISPLAY etc.; NW_AUTH_PASS gone) */
	perror("nanosu: exec");
	return 127;
}

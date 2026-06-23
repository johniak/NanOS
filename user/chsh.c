/*
 * chsh.c — change a user's login shell (the 7th /etc/passwd field), the Linux way.
 *
 *   chsh [-s SHELL] [USER]
 *
 * The login shell is selected exactly as on Linux: it is stored per user in /etc/passwd field 7
 * (pw_shell), which login/su read via getpwnam() and exec. chsh edits that field. A non-root user
 * may change only their OWN shell, and only to a shell listed in /etc/shells (the registry of
 * valid login shells). Installed setuid-root so an unprivileged user can rewrite the account DB.
 *
 * Persistence: NanOS keeps the canonical passwd on the disk at /disks/main/nanos/config/passwd and
 * mirrors it into the writable /etc tmpfs at boot. chsh writes BOTH — the disk copy so the change
 * survives reboot, and the live /etc/passwd so getpwnam() sees it immediately this session.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>

#define DISK_PASSWD "/disks/main/nanos/config/passwd"
#define ETC_PASSWD  "/etc/passwd"

/* True if `sh` is a permitted login shell (appears, uncommented, in /etc/shells). */
static int shell_listed(const char* sh)
{
	FILE* f = fopen("/etc/shells", "r");
	if (!f)
		return 0;
	char line[256];
	int ok = 0;
	while (fgets(line, sizeof line, f)) {
		char* p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '#' || *p == '\n' || *p == 0) continue;
		char* e = p + strlen(p);
		while (e > p && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
		if (strcmp(p, sh) == 0) { ok = 1; break; }
	}
	fclose(f);
	return ok;
}

/* Rewrite `path`, replacing the shell field of `user`'s line with `newsh`. Returns 0 on success,
 * -1 on I/O error, 1 if the user was not found. */
static int rewrite(const char* path, const char* user, const char* newsh)
{
	FILE* f = fopen(path, "r");
	if (!f)
		return -1;
	static char out[16384];
	int n = 0, found = 0, ulen = (int) strlen(user);
	char line[512];
	while (fgets(line, sizeof line, f)) {
		if (strncmp(line, user, ulen) == 0 && line[ulen] == ':') {
			/* split into the 7 colon fields (trim the trailing newline first) */
			char buf[512];
			strncpy(buf, line, sizeof buf - 1); buf[sizeof buf - 1] = 0;
			char* nl = strpbrk(buf, "\r\n"); if (nl) *nl = 0;
			char* fld[7]; int i = 0; char* s = buf;
			fld[i++] = s;
			for (; *s && i < 7; s++) if (*s == ':') { *s = 0; fld[i++] = s + 1; }
			if (i == 7) {
				n += snprintf(out + n, sizeof out - n, "%s:%s:%s:%s:%s:%s:%s\n",
					fld[0], fld[1], fld[2], fld[3], fld[4], fld[5], newsh);
				found = 1;
				continue;
			}
		}
		n += snprintf(out + n, sizeof out - n, "%s", line);
	}
	fclose(f);
	if (!found)
		return 1;
	f = fopen(path, "w");
	if (!f)
		return -1;
	fwrite(out, 1, (size_t) n, f);
	fclose(f);
	return 0;
}

int main(int argc, char** argv)
{
	const char* newsh = 0;
	const char* user = 0;
	int i;
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) newsh = argv[++i];
		else if (argv[i][0] != '-') user = argv[i];
	}

	struct passwd* me = getpwuid(getuid());
	if (!user)
		user = me ? me->pw_name : 0;
	if (!user) {
		fprintf(stderr, "chsh: cannot determine the current user\n");
		return 1;
	}

	/* A non-root user may change only their own shell. */
	if (getuid() != 0 && (!me || strcmp(user, me->pw_name) != 0)) {
		fprintf(stderr, "chsh: you may only change your own shell\n");
		return 1;
	}

	if (!newsh) {
		struct passwd* pw = getpwnam(user);
		printf("Changing the login shell for %s\n", user);
		printf("Enter the new value, or press ENTER for the default\n");
		printf("\tLogin Shell [%s]: ", pw && pw->pw_shell ? pw->pw_shell : "");
		fflush(stdout);
		static char buf[256];
		if (!fgets(buf, sizeof buf, stdin)) return 0;
		char* nl = strpbrk(buf, "\r\n"); if (nl) *nl = 0;
		if (buf[0] == 0) return 0;        /* empty -> keep current */
		newsh = buf;
	}

	if (!shell_listed(newsh)) {
		fprintf(stderr, "chsh: %s is not an authorized shell (see /etc/shells)\n", newsh);
		return 1;
	}

	/* Persist to the on-disk account DB, then mirror into the live /etc copy so getpwnam() sees
	 * it immediately. (/etc/passwd may be absent on a minimal boot; that is not fatal.) */
	int rc = rewrite(DISK_PASSWD, user, newsh);
	if (rc == 1) { fprintf(stderr, "chsh: unknown user %s\n", user); return 1; }
	if (rc < 0)  { fprintf(stderr, "chsh: cannot write %s (permission denied?)\n", DISK_PASSWD); return 1; }
	rewrite(ETC_PASSWD, user, newsh);

	printf("Shell changed to %s\n", newsh);
	return 0;
}

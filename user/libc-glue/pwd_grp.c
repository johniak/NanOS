/*
 * pwd_grp.c — getpwuid/getpwnam/getgrgid (picolibc declares <pwd.h>/<grp.h> but provides no
 * implementation). Like glibc reads /etc/passwd, we read the account database — but NanOS
 * keeps system config under /nanos/config (not /etc), so it lives at
 * /disks/main/nanos/config/passwd. The 7th field is the login shell, so editing that file
 * (NanOS's read-only-disk `chsh`) sets the default shell init and the terminal launch.
 *
 * If the file cannot be read we fall back to a built-in root entry so the system still comes
 * up.
 */
#include <pwd.h>
#include <grp.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* /etc/passwd is a symlink to the persistent DB at /disks/main/nanos/config/passwd. Field 2
 * is "x" (the password hash lives in /etc/shadow; see grp_shadow.c). */
#define PASSWD_PATH "/etc/passwd"

/* Storage for the most recent lookup: the parsed line (fields point into it) + the struct.
 * Matches the classic getpwnam contract — the returned pointer is valid until the next call. */
static char g_line[256];
static struct passwd g_pw;

/* Built-in root, used when /etc/passwd is unreadable. nsh is the always-present shell. */
static struct passwd* fallback_root(void) {
	g_pw.pw_name = (char*) "root";
	g_pw.pw_passwd = (char*) "x";
	g_pw.pw_uid = 0;
	g_pw.pw_gid = 0;
	g_pw.pw_comment = (char*) "";
	g_pw.pw_gecos = (char*) "root";
	g_pw.pw_dir = (char*) "/";
	g_pw.pw_shell = (char*) "/disks/main/nanos/bin/nsh.nxe";
	return &g_pw;
}

/* Split g_line in place on ':' into the 7 passwd fields; returns 1 on success. A trailing
 * newline on the shell field is trimmed. Empty string fields stay non-NULL (""). */
static int parse_line(void) {
	char* f[7];
	int i = 0;
	char* p = g_line;
	f[i++] = p;
	for (; *p && i < 7; p++) {
		if (*p == ':') { *p = 0; f[i++] = p + 1; }
	}
	if (i < 7)
		return 0;
	for (char* q = f[6]; *q; q++)
		if (*q == '\n' || *q == '\r') { *q = 0; break; }
	g_pw.pw_name = f[0];
	g_pw.pw_passwd = f[1];
	g_pw.pw_uid = (uid_t) atoi(f[2]);
	g_pw.pw_gid = (gid_t) atoi(f[3]);
	g_pw.pw_comment = (char*) "";
	g_pw.pw_gecos = f[4];
	g_pw.pw_dir = f[5];
	g_pw.pw_shell = (f[6][0] ? f[6] : (char*) "/disks/main/nanos/bin/nsh.nxe");
	return 1;
}

/* Scan /etc/passwd for the entry matching either uid (by_uid) or name. */
static struct passwd* lookup(int by_uid, uid_t uid, const char* name) {
	FILE* fp = fopen(PASSWD_PATH, "r");
	if (!fp)
		return (by_uid ? (uid == 0 ? fallback_root() : 0)
		               : (name && strcmp(name, "root") == 0 ? fallback_root() : 0));
	while (fgets(g_line, sizeof g_line, fp)) {
		if (!parse_line())
			continue;
		if (by_uid ? (g_pw.pw_uid == uid)
		           : (name && strcmp(g_pw.pw_name, name) == 0)) {
			fclose(fp);
			return &g_pw;
		}
	}
	fclose(fp);
	return 0;
}

struct passwd* getpwuid(uid_t uid) { return lookup(1, uid, 0); }
struct passwd* getpwnam(const char* name) { return lookup(0, 0, name); }

/* Reentrant variants (toybox/login use them): look up via the non-_r form, then deep-copy the
 * strings into the caller's buffer so the result outlives the next lookup. Returns 0 on success
 * (*result -> pwd), ENOENT-as-0-with-NULL-result if absent, ERANGE if buf is too small. */
static int pw_copy(struct passwd* src, struct passwd* pwd, char* buf, size_t buflen,
                   struct passwd** result) {
	if (!src) { *result = 0; return 0; }
	const char* fields[5] = { src->pw_name, src->pw_passwd, src->pw_gecos, src->pw_dir, src->pw_shell };
	size_t need = 0;
	for (int i = 0; i < 5; i++) need += strlen(fields[i] ? fields[i] : "") + 1;
	if (need > buflen) { *result = 0; return ERANGE; }
	char* p = buf;
	char** dst[5] = { &pwd->pw_name, &pwd->pw_passwd, &pwd->pw_gecos, &pwd->pw_dir, &pwd->pw_shell };
	for (int i = 0; i < 5; i++) {
		const char* s = fields[i] ? fields[i] : "";
		size_t l = strlen(s) + 1;
		memcpy(p, s, l);
		*dst[i] = p;
		p += l;
	}
	pwd->pw_uid = src->pw_uid;
	pwd->pw_gid = src->pw_gid;
	pwd->pw_comment = (char*) "";
	*result = pwd;
	return 0;
}
int getpwnam_r(const char* name, struct passwd* pwd, char* buf, size_t buflen, struct passwd** result) {
	return pw_copy(getpwnam(name), pwd, buf, buflen, result);
}
int getpwuid_r(uid_t uid, struct passwd* pwd, char* buf, size_t buflen, struct passwd** result) {
	return pw_copy(getpwuid(uid), pwd, buf, buflen, result);
}

/* getgrnam/getgrgid/getgrent + getspnam + getgrouplist/initgroups live in grp_shadow.c
 * (they parse /etc/group and /etc/shadow with member lists). */

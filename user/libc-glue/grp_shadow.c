/*
 * grp_shadow.c — /etc/group and /etc/shadow access for NanOS (picolibc ships <grp.h> but no
 * implementation, and no <shadow.h> at all — we add one under libc-glue/include). Clean-room,
 * modeled on the POSIX contracts; reads the colon-separated databases the same way glibc does.
 *
 *   /etc/group   name:passwd:gid:member,member,...
 *   /etc/shadow  name:$6$salt$hash:lastchg:min:max:warn:inact:expire:flag
 *
 * Both are symlinks into the persistent DB at /disks/main/nanos/config/. The returned pointers
 * are valid until the next call (classic getgrnam/getspnam contract).
 */
#include <grp.h>
#include <shadow.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GROUP_PATH  "/etc/group"
#define SHADOW_PATH "/etc/shadow"
#define MAX_MEMBERS 64
#define NGROUPS_GLUE 32   /* matches the kernel's NGROUPS_MAX */

/* picolibc's <grp.h> declares none of these; provide the prototypes (setgroups is defined in
 * syscalls.c, getgrouplist/initgroups below). */
int setgroups(int n, const gid_t* list);
int getgrouplist(const char* user, gid_t group, gid_t* groups, int* ngroups);
int initgroups(const char* user, gid_t group);

/* ---- /etc/group --------------------------------------------------------------------------- */

static char        g_grline[512];
static struct group g_gr;
static char*       g_grmem[MAX_MEMBERS + 1];
static FILE*       g_grfp;

/* Parse g_grline in place into g_gr (name:passwd:gid:csv-members). Returns 1 on success. */
static int parse_group(void) {
	char* f[4];
	int i = 0;
	char* p = g_grline;
	f[i++] = p;
	for (; *p && i < 4; p++)
		if (*p == ':') { *p = 0; f[i++] = p + 1; }
	if (i < 4) return 0;
	/* trim trailing newline on the member field */
	for (char* q = f[3]; *q; q++)
		if (*q == '\n' || *q == '\r') { *q = 0; break; }
	g_gr.gr_name = f[0];
	g_gr.gr_passwd = f[1];
	g_gr.gr_gid = (gid_t) atoi(f[2]);
	/* split the comma-separated member list */
	int m = 0;
	if (f[3][0]) {
		char* s = f[3];
		g_grmem[m++] = s;
		for (; *s && m < MAX_MEMBERS; s++)
			if (*s == ',') { *s = 0; g_grmem[m++] = s + 1; }
	}
	g_grmem[m] = 0;
	g_gr.gr_mem = g_grmem;
	return 1;
}

void setgrent(void) { if (g_grfp) fclose(g_grfp); g_grfp = fopen(GROUP_PATH, "r"); }
void endgrent(void) { if (g_grfp) { fclose(g_grfp); g_grfp = 0; } }

struct group* getgrent(void) {
	if (!g_grfp) { g_grfp = fopen(GROUP_PATH, "r"); if (!g_grfp) return 0; }
	while (fgets(g_grline, sizeof g_grline, g_grfp))
		if (parse_group()) return &g_gr;
	return 0;
}

static struct group* grlookup(int by_gid, gid_t gid, const char* name) {
	FILE* fp = fopen(GROUP_PATH, "r");
	if (!fp) return 0;
	while (fgets(g_grline, sizeof g_grline, fp)) {
		if (!parse_group()) continue;
		if (by_gid ? (g_gr.gr_gid == gid) : (name && strcmp(g_gr.gr_name, name) == 0)) {
			fclose(fp);
			return &g_gr;
		}
	}
	fclose(fp);
	return 0;
}

struct group* getgrgid(gid_t gid) { return grlookup(1, gid, 0); }
struct group* getgrnam(const char* name) { return grlookup(0, 0, name); }

/* ---- /etc/shadow -------------------------------------------------------------------------- */

static char       g_spline[512];
static struct spwd g_sp;

static int parse_shadow(void) {
	char* f[9];
	int i = 0;
	char* p = g_spline;
	f[i++] = p;
	for (; *p && i < 9; p++)
		if (*p == ':') { *p = 0; f[i++] = p + 1; }
	if (i < 2) return 0;                      /* need at least name + hash */
	for (char* q = f[i - 1]; *q; q++)
		if (*q == '\n' || *q == '\r') { *q = 0; break; }
	g_sp.sp_namp = f[0];
	g_sp.sp_pwdp = f[1];
	g_sp.sp_lstchg = (i > 2 && f[2][0]) ? atol(f[2]) : -1;
	g_sp.sp_min    = (i > 3 && f[3][0]) ? atol(f[3]) : -1;
	g_sp.sp_max    = (i > 4 && f[4][0]) ? atol(f[4]) : -1;
	g_sp.sp_warn   = (i > 5 && f[5][0]) ? atol(f[5]) : -1;
	g_sp.sp_inact  = (i > 6 && f[6][0]) ? atol(f[6]) : -1;
	g_sp.sp_expire = (i > 7 && f[7][0]) ? atol(f[7]) : -1;
	g_sp.sp_flag   = (i > 8 && f[8][0]) ? (unsigned long) atol(f[8]) : ~0UL;
	return 1;
}

struct spwd* getspnam(const char* name) {
	FILE* fp = fopen(SHADOW_PATH, "r");
	if (!fp) return 0;
	while (fgets(g_spline, sizeof g_spline, fp)) {
		if (!parse_shadow()) continue;
		if (name && strcmp(g_sp.sp_namp, name) == 0) { fclose(fp); return &g_sp; }
	}
	fclose(fp);
	return 0;
}

/* shadow enumeration (sudo opens it around getspnam). */
static FILE* g_spfp;
void setspent(void) { if (g_spfp) fclose(g_spfp); g_spfp = fopen(SHADOW_PATH, "r"); }
void endspent(void) { if (g_spfp) { fclose(g_spfp); g_spfp = 0; } }
struct spwd* getspent(void) {
	if (!g_spfp) { g_spfp = fopen(SHADOW_PATH, "r"); if (!g_spfp) return 0; }
	while (fgets(g_spline, sizeof g_spline, g_spfp))
		if (parse_shadow()) return &g_sp;
	return 0;
}

/* ---- supplementary groups ----------------------------------------------------------------- */

/* A user's groups: their primary `group` gid plus every /etc/group that lists `user` as a member.
 * Fills *groups (capacity *ngroups). Returns the number written, or -1 if it didn't fit (and sets
 * *ngroups to the count needed) — the glibc contract. */
int getgrouplist(const char* user, gid_t group, gid_t* groups, int* ngroups) {
	int cap = *ngroups;
	int n = 0;
	if (n < cap && groups) groups[n] = group;
	n++;
	FILE* fp = fopen(GROUP_PATH, "r");
	if (fp) {
		while (fgets(g_grline, sizeof g_grline, fp)) {
			if (!parse_group()) continue;
			if (g_gr.gr_gid == group) continue;            /* already counted as primary */
			int isMember = 0;
			for (int m = 0; g_gr.gr_mem[m]; m++)
				if (strcmp(g_gr.gr_mem[m], user) == 0) { isMember = 1; break; }
			if (!isMember) continue;
			if (n < cap && groups) groups[n] = g_gr.gr_gid;
			n++;
		}
		fclose(fp);
	}
	*ngroups = n;
	return (n <= cap) ? n : -1;
}

/* setgroups() the caller's supplementary groups (requires root — login/su call this while still
 * privileged, before setuid). */
int initgroups(const char* user, gid_t group) {
	gid_t list[NGROUPS_GLUE];
	int n = NGROUPS_GLUE;
	if (getgrouplist(user, group, list, &n) < 0)
		n = NGROUPS_GLUE;                                  /* truncate to what we can carry */
	return setgroups(n, list);
}

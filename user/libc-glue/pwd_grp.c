/*
 * pwd_grp.c — getpwuid/getgrgid (picolibc declares <pwd.h>/<grp.h> but provides no
 * implementation). NanOS is single-user: the only account/group is root (id 0), and the
 * kernel stamps every file uid/gid 0. So we resolve id 0 to "root" and report any other
 * id as "not found" (NULL) — the POSIX contract — instead of falsely naming every id
 * "root"/"wheel". ls -l then prints the numeric id for an unknown owner, as on Linux.
 *
 * EVERY string field is non-NULL: a hosted shell (bash) reads pw_dir/pw_shell at startup and
 * `savestring()`s them unconditionally (strlen on a NULL pw_dir = crash). Home is "/" (the
 * always-present read-only root) and the login shell is the bash bundle in the link farm.
 */
#include <pwd.h>
#include <grp.h>

struct passwd* getpwuid(uid_t uid) {
	if (uid != 0)
		return 0;                 // no such user (only root exists)
	static struct passwd pw;
	pw.pw_name = (char*) "root";
	pw.pw_passwd = (char*) "x";
	pw.pw_uid = 0;
	pw.pw_gid = 0;
	pw.pw_comment = (char*) "";
	pw.pw_gecos = (char*) "root";
	pw.pw_dir = (char*) "/";
	pw.pw_shell = (char*) "/disks/main/bin/bash.nxe";
	return &pw;
}

struct group* getgrgid(gid_t gid) {
	if (gid != 0)
		return 0;                 // no such group (only the root group exists)
	static struct group gr;
	static char* members[] = { 0 };
	gr.gr_name = (char*) "root";  // gid 0 is "root" on Linux (BSD names it "wheel")
	gr.gr_passwd = (char*) "x";
	gr.gr_gid = 0;
	gr.gr_mem = members;          // empty, NULL-terminated (bash iterates this)
	return &gr;
}

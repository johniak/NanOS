/*
 * pwd_grp.c — getpwuid/getgrgid (picolibc declares <pwd.h>/<grp.h> but provides no
 * implementation). NanOS is single-user: the only account/group is root (id 0), and the
 * kernel stamps every file uid/gid 0. So we resolve id 0 to "root" and report any other
 * id as "not found" (NULL) — the POSIX contract — instead of falsely naming every id
 * "root"/"wheel". ls -l then prints the numeric id for an unknown owner, as on Linux.
 */
#include <pwd.h>
#include <grp.h>

struct passwd* getpwuid(uid_t uid) {
	if (uid != 0)
		return 0;                 // no such user (only root exists)
	static struct passwd pw;
	pw.pw_name = (char*) "root";
	pw.pw_uid = 0;
	pw.pw_gid = 0;
	return &pw;
}

struct group* getgrgid(gid_t gid) {
	if (gid != 0)
		return 0;                 // no such group (only the root group exists)
	static struct group gr;
	gr.gr_name = (char*) "root";  // gid 0 is "root" on Linux (BSD names it "wheel")
	gr.gr_gid = 0;
	return &gr;
}

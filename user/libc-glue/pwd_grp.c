/*
 * pwd_grp.c — getpwuid/getgrgid stubs (picolibc declares <pwd.h>/<grp.h> but
 * provides no implementation). NanOS has no user/group name database, so every
 * uid maps to "root" and every gid to "wheel". ls -l reads pw_name/gr_name.
 */
#include <pwd.h>
#include <grp.h>

struct passwd* getpwuid(uid_t uid) {
	static struct passwd pw;
	pw.pw_name = (char*) "root";
	pw.pw_uid = uid;
	pw.pw_gid = 0;
	return &pw;
}

struct group* getgrgid(gid_t gid) {
	static struct group gr;
	gr.gr_name = (char*) "wheel";
	gr.gr_gid = gid;
	return &gr;
}

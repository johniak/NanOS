/* mntent.h — /etc/mtab entry API. Stub for ports (busybox libbb.h includes it unconditionally);
 * NanOS has no mount table, so these are decls only (udhcpc never calls them -> never linked). */
#ifndef _NANOS_MNTENT_H
#define _NANOS_MNTENT_H
#include <stdio.h>
#define MOUNTED "/etc/mtab"
#define MNTTYPE_IGNORE "ignore"
struct mntent {
	char* mnt_fsname; char* mnt_dir; char* mnt_type; char* mnt_opts;
	int mnt_freq; int mnt_passno;
};
#ifdef __cplusplus
extern "C" {
#endif
FILE* setmntent(const char* filename, const char* type);
struct mntent* getmntent(FILE* stream);
struct mntent* getmntent_r(FILE* stream, struct mntent* result, char* buf, int bufsize);
int addmntent(FILE* stream, const struct mntent* mnt);
int endmntent(FILE* stream);
char* hasmntopt(const struct mntent* mnt, const char* opt);
#ifdef __cplusplus
}
#endif
#endif

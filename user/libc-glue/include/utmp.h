/*
 * utmp.h — login-records surface (glibc i686 struct utmp layout). NanOS keeps no utmp/wtmp
 * database, but inetutils' shared libinetutils references struct utmp + PATH_WTMP at compile
 * time for its (disabled) login/wtmp daemons. These declarations let that code compile; the
 * functions are unimplemented and unlinked by ping.
 */
#ifndef _UTMP_H
#define _UTMP_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UT_LINESIZE 32
#define UT_NAMESIZE 32
#define UT_HOSTSIZE 256

/* ut_type values (Linux). */
#define EMPTY         0
#define RUN_LVL       1
#define BOOT_TIME     2
#define NEW_TIME      3
#define OLD_TIME      4
#define INIT_PROCESS  5
#define LOGIN_PROCESS 6
#define USER_PROCESS  7
#define DEAD_PROCESS  8
#define ACCOUNTING    9

#define PATH_UTMP "/var/run/utmp"
#define PATH_WTMP "/var/log/wtmp"
#define _PATH_UTMP PATH_UTMP
#define _PATH_WTMP PATH_WTMP

struct exit_status {
	short int e_termination;
	short int e_exit;
};

struct utmp {
	short int ut_type;
	pid_t     ut_pid;
	char      ut_line[UT_LINESIZE];
	char      ut_id[4];
	char      ut_user[UT_NAMESIZE];
	char      ut_host[UT_HOSTSIZE];
	struct exit_status ut_exit;
	long int  ut_session;
	struct {
		int tv_sec;
		int tv_usec;
	} ut_tv;
	int       ut_addr_v6[4];
	char      __glibc_reserved[20];
};

#define ut_name ut_user
#define ut_time ut_tv.tv_sec
#define ut_addr ut_addr_v6[0]

void          setutent(void);
void          endutent(void);
struct utmp*  getutent(void);
struct utmp*  getutid(const struct utmp* id);
struct utmp*  getutline(const struct utmp* line);
struct utmp*  pututline(const struct utmp* ut);
int           utmpname(const char* file);
void          login(const struct utmp* ut);
int           logout(const char* ut_line);
void          logwtmp(const char* line, const char* name, const char* host);

#ifdef __cplusplus
}
#endif

#endif /* _UTMP_H */

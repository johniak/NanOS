/*
 * shadow.h — minimal /etc/shadow access (picolibc ships none). Matches the glibc layout the
 * NanOS tools (toybox login/su/passwd, sudo) compile against. Implementation: grp_shadow.c.
 */
#ifndef _SHADOW_H
#define _SHADOW_H

#ifdef __cplusplus
extern "C" {
#endif

struct spwd {
	char*         sp_namp;    /* login name */
	char*         sp_pwdp;    /* encrypted password ($6$...) */
	long          sp_lstchg;  /* days since epoch of last change */
	long          sp_min;     /* min days between changes */
	long          sp_max;     /* max days password is valid */
	long          sp_warn;    /* days before expiry to warn */
	long          sp_inact;   /* days after expiry until account is disabled */
	long          sp_expire;  /* days since epoch when the account expires */
	unsigned long sp_flag;    /* reserved */
};

struct spwd* getspnam(const char* name);

#ifdef __cplusplus
}
#endif

#endif /* _SHADOW_H */

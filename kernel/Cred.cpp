#include "Cred.h"

// Standalone errno values (guarded so this compiles whether or not Syscall.h is also included).
#ifndef EPERM
#define EPERM 1
#endif
#ifndef EACCES
#define EACCES 13
#endif
#ifndef EINVAL
#define EINVAL 22
#endif

namespace kernel {

void credInitRoot(Cred& c) {
	c.ruid = c.euid = c.suid = c.fsuid = 0;
	c.rgid = c.egid = c.sgid = c.fsgid = 0;
	c.ngroups = 0;
	for (int i = 0; i < NGROUPS_MAX; i++) c.groups[i] = 0;
}

bool credInGroup(const Cred& c, unsigned gid) {
	if (c.egid == gid || c.fsgid == gid) return true;
	for (int i = 0; i < c.ngroups; i++)
		if (c.groups[i] == gid) return true;
	return false;
}

// access(2) checks real gid + supplementary; effective checks egid/fsgid + supplementary.
static bool credInGroupAs(const Cred& c, unsigned gid, bool useReal) {
	if (useReal) { if (c.rgid == gid) return true; }
	else         { if (c.egid == gid || c.fsgid == gid) return true; }
	for (int i = 0; i < c.ngroups; i++) if (c.groups[i] == gid) return true;
	return false;
}

int credAccess(const Cred& c, unsigned fileUid, unsigned fileGid, unsigned mode, int want, bool useReal) {
	if (want == 0) return 0;
	unsigned myUid = useReal ? c.ruid : c.fsuid;
	// Root override follows the SAME id set as the check: access(2) (useReal) decides by the
	// real uid, so a setuid-root program checks as the real user; normal opens use fsuid.
	bool isRoot = useReal ? (c.ruid == 0) : (c.euid == 0);
	if (isRoot) {
		if ((want & 1) && (mode & 0111) == 0) return -EACCES;   // x still needs a bit somewhere
		return 0;
	}
	unsigned bits;
	if (fileUid == myUid)                          bits = (mode >> 6) & 7;   // owner
	else if (credInGroupAs(c, fileGid, useReal))   bits = (mode >> 3) & 7;   // group
	else                                           bits = mode & 7;          // other
	return ((bits & (unsigned) want) == (unsigned) want) ? 0 : -EACCES;
}

int credMaySticky(const Cred& c, unsigned dirUid, unsigned fileUid) {
	if (c.euid == 0 || c.fsuid == fileUid || c.fsuid == dirUid) return 0;
	return -EPERM;
}
int credMayChmod(const Cred& c, unsigned fileUid) {
	return (c.euid == 0 || c.fsuid == fileUid) ? 0 : -EPERM;
}
int credMayChown(const Cred& c, unsigned fileUid, int newUid, int newGid) {
	if (c.euid == 0) return 0;
	if (newUid != -1 && (unsigned) newUid != fileUid) return -EPERM;  // chown owner = root only
	if (c.fsuid != fileUid) return -EPERM;                            // must own the file
	if (newGid != -1 && !credInGroup(c, (unsigned) newGid)) return -EPERM;
	return 0;
}
int credMayUtimes(const Cred& c, unsigned fileUid, bool toNow) {
	if (c.euid == 0 || c.fsuid == fileUid) return 0;
	if (toNow) return 0;          // utimes(NULL): write perm is checked separately by the VFS
	return -EPERM;
}

void credOnExec(Cred& c, unsigned fileUid, unsigned fileGid, unsigned mode) {
	if (mode & 04000) { c.euid = c.fsuid = fileUid; }   // S_ISUID
	if (mode & 02000) { c.egid = c.fsgid = fileGid; }   // S_ISGID
	c.suid = c.euid; c.sgid = c.egid;                   // Linux: saved sets = effective on exec
}

static bool priv(const Cred& c) { return c.euid == 0; }
static bool oneOfRES_u(const Cred& c, unsigned v) { return v == c.ruid || v == c.euid || v == c.suid; }
static bool oneOfRES_g(const Cred& c, unsigned v) { return v == c.rgid || v == c.egid || v == c.sgid; }

int credSetuid(Cred& c, unsigned uid) {
	if (priv(c)) { c.ruid = c.euid = c.suid = c.fsuid = uid; return 0; }
	if (uid == c.ruid || uid == c.suid) { c.euid = c.fsuid = uid; return 0; }
	return -EPERM;
}
int credSeteuid(Cred& c, unsigned euid) {
	if (priv(c) || oneOfRES_u(c, euid)) { c.euid = c.fsuid = euid; return 0; }
	return -EPERM;
}
int credSetreuid(Cred& c, int ruid, int euid) {
	unsigned nr = (ruid == -1) ? c.ruid : (unsigned) ruid;
	unsigned ne = (euid == -1) ? c.euid : (unsigned) euid;
	if (!priv(c)) {
		if (ruid != -1 && nr != c.ruid && nr != c.euid) return -EPERM;
		if (euid != -1 && ne != c.ruid && ne != c.euid && ne != c.suid) return -EPERM;
	}
	bool ruidChanged = (ruid != -1 && nr != c.ruid);
	bool euidToNonRuid = (euid != -1 && ne != c.ruid);
	c.ruid = nr; c.euid = ne; c.fsuid = ne;
	if (ruidChanged || euidToNonRuid) c.suid = c.euid;   // POSIX: suid tracks euid here
	return 0;
}
int credSetresuid(Cred& c, int r, int e, int s) {
	unsigned nr = (r == -1) ? c.ruid : (unsigned) r;
	unsigned ne = (e == -1) ? c.euid : (unsigned) e;
	unsigned ns = (s == -1) ? c.suid : (unsigned) s;
	if (!priv(c)) {
		if (r != -1 && !oneOfRES_u(c, nr)) return -EPERM;
		if (e != -1 && !oneOfRES_u(c, ne)) return -EPERM;
		if (s != -1 && !oneOfRES_u(c, ns)) return -EPERM;
	}
	c.ruid = nr; c.euid = ne; c.suid = ns; c.fsuid = ne;   // fsuid follows euid
	return 0;
}
unsigned credSetfsuid(Cred& c, unsigned fsuid) {
	unsigned prev = c.fsuid;
	if (priv(c) || fsuid == c.ruid || fsuid == c.euid || fsuid == c.suid || fsuid == c.fsuid)
		c.fsuid = fsuid;
	return prev;
}

int credSetgid(Cred& c, unsigned gid) {
	if (priv(c)) { c.rgid = c.egid = c.sgid = c.fsgid = gid; return 0; }
	if (gid == c.rgid || gid == c.sgid) { c.egid = c.fsgid = gid; return 0; }
	return -EPERM;
}
int credSetegid(Cred& c, unsigned egid) {
	if (priv(c) || oneOfRES_g(c, egid)) { c.egid = c.fsgid = egid; return 0; }
	return -EPERM;
}
int credSetregid(Cred& c, int rgid, int egid) {
	unsigned nr = (rgid == -1) ? c.rgid : (unsigned) rgid;
	unsigned ne = (egid == -1) ? c.egid : (unsigned) egid;
	if (!priv(c)) {
		if (rgid != -1 && nr != c.rgid && nr != c.egid) return -EPERM;
		if (egid != -1 && ne != c.rgid && ne != c.egid && ne != c.sgid) return -EPERM;
	}
	bool rch = (rgid != -1 && nr != c.rgid);
	bool enr = (egid != -1 && ne != c.rgid);
	c.rgid = nr; c.egid = ne; c.fsgid = ne;
	if (rch || enr) c.sgid = c.egid;
	return 0;
}
int credSetresgid(Cred& c, int r, int e, int s) {
	unsigned nr = (r == -1) ? c.rgid : (unsigned) r;
	unsigned ne = (e == -1) ? c.egid : (unsigned) e;
	unsigned ns = (s == -1) ? c.sgid : (unsigned) s;
	if (!priv(c)) {
		if (r != -1 && !oneOfRES_g(c, nr)) return -EPERM;
		if (e != -1 && !oneOfRES_g(c, ne)) return -EPERM;
		if (s != -1 && !oneOfRES_g(c, ns)) return -EPERM;
	}
	c.rgid = nr; c.egid = ne; c.sgid = ns; c.fsgid = ne;
	return 0;
}
unsigned credSetfsgid(Cred& c, unsigned fsgid) {
	unsigned prev = c.fsgid;
	if (priv(c) || fsgid == c.rgid || fsgid == c.egid || fsgid == c.sgid || fsgid == c.fsgid)
		c.fsgid = fsgid;
	return prev;
}
int credSetgroups(Cred& c, const unsigned* g, int n) {
	if (!priv(c)) return -EPERM;
	if (n < 0 || n > NGROUPS_MAX) return -EINVAL;
	c.ngroups = n;
	for (int i = 0; i < n; i++) c.groups[i] = g[i];
	return 0;
}

} // namespace kernel

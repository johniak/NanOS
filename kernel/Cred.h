#ifndef CRED_H_
#define CRED_H_

namespace kernel {

static const int NGROUPS_MAX = 32;

// Process credentials — the canonical identity (Linux task_struct->cred analogue).
// Plain data; all policy lives in the pure functions below so it is host-testable.
struct Cred {
	unsigned ruid, euid, suid, fsuid;   // real / effective / saved-set / filesystem uid
	unsigned rgid, egid, sgid, fsgid;   // ... gids
	int      ngroups;                   // valid entries in groups[]
	unsigned groups[NGROUPS_MAX];       // supplementary groups
};

void credInitRoot(Cred& c);                              // all ids 0, no supplementary groups
bool credInGroup(const Cred& c, unsigned gid);           // egid/fsgid + supplementary scan

// Access decision. want = R(4)|W(2)|X(1). useReal selects ruid/rgid (access(2)) vs fsuid/fsgid.
// Returns 0 if allowed, -EACCES otherwise. Root (euid==0): r/w always; x iff any exec bit set.
int credAccess(const Cred& c, unsigned fileUid, unsigned fileGid, unsigned mode, int want, bool useReal);

// Ownership / sticky / metadata rules. Return 0 or -EPERM.
int credMaySticky(const Cred& c, unsigned dirUid, unsigned fileUid);   // S_ISVTX delete/rename
int credMayChmod(const Cred& c, unsigned fileUid);
int credMayChown(const Cred& c, unsigned fileUid, int newUid, int newGid);
int credMayUtimes(const Cred& c, unsigned fileUid, bool toNow);

// Apply setuid/setgid bits at execve. Always sets saved sets to the (possibly raised) effective.
void credOnExec(Cred& c, unsigned fileUid, unsigned fileGid, unsigned mode);

// Transitions. Each mutates c in place and returns 0, or returns -EPERM and leaves c unchanged.
// A -1 argument means "leave that id unchanged" (POSIX setreuid/setresuid convention).
int credSetuid(Cred& c, unsigned uid);
int credSeteuid(Cred& c, unsigned euid);
int credSetreuid(Cred& c, int ruid, int euid);
int credSetresuid(Cred& c, int r, int e, int s);
unsigned credSetfsuid(Cred& c, unsigned fsuid);    // returns the PREVIOUS fsuid (Linux semantics)
int credSetgid(Cred& c, unsigned gid);
int credSetegid(Cred& c, unsigned egid);
int credSetregid(Cred& c, int rgid, int egid);
int credSetresgid(Cred& c, int r, int e, int s);
unsigned credSetfsgid(Cred& c, unsigned fsgid);
int credSetgroups(Cred& c, const unsigned* g, int n);   // root-only; -EPERM/-EINVAL else

}
#endif /* CRED_H_ */

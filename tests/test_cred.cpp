#include "doctest.h"
#include "Cred.h"
using namespace kernel;

TEST_CASE("credInitRoot makes a full-root credential") {
	Cred c; credInitRoot(c);
	CHECK(c.ruid == 0); CHECK(c.euid == 0); CHECK(c.suid == 0); CHECK(c.fsuid == 0);
	CHECK(c.rgid == 0); CHECK(c.egid == 0); CHECK(c.sgid == 0); CHECK(c.fsgid == 0);
	CHECK(c.ngroups == 0);
}

TEST_CASE("credInGroup matches egid, fsgid, and supplementary groups") {
	Cred c; credInitRoot(c);
	c.egid = 5; c.fsgid = 5; c.ngroups = 2; c.groups[0] = 10; c.groups[1] = 11;
	CHECK(credInGroup(c, 5));      // egid/fsgid
	CHECK(credInGroup(c, 10));     // supplementary
	CHECK(credInGroup(c, 11));
	CHECK_FALSE(credInGroup(c, 99));
}

TEST_CASE("credAccess: owner/group/other selection by fsuid/fsgid") {
	Cred c; credInitRoot(c);
	c.ruid = c.euid = c.suid = c.fsuid = 1000; c.rgid = c.egid = c.sgid = c.fsgid = 1000;
	CHECK(credAccess(c, 1000, 1000, 0640, 4, false) == 0);     // owner read ok
	CHECK(credAccess(c, 1000, 1000, 0640, 2, false) == 0);     // owner write ok
	CHECK(credAccess(c, 1000, 1000, 0640, 1, false) == -13);   // no exec bit -> EACCES
	CHECK(credAccess(c, 0, 0, 0600, 4, false) == -13);         // other has nothing
	CHECK(credAccess(c, 0, 1000, 0040, 4, false) == 0);        // group read via membership
}

TEST_CASE("credAccess: root bypasses r/w but needs an exec bit for x") {
	Cred c; credInitRoot(c);   // euid 0
	CHECK(credAccess(c, 1000, 1000, 0600, 6, false) == 0);   // rw on someone else's 0600
	CHECK(credAccess(c, 1000, 1000, 0600, 1, false) == -13); // x but no exec bits anywhere
	CHECK(credAccess(c, 1000, 1000, 0700, 1, false) == 0);   // an exec bit exists
}

TEST_CASE("credAccess: useReal uses ruid/rgid (access(2))") {
	Cred c; credInitRoot(c);
	c.ruid = 1000; c.euid = 0; c.fsuid = 0; c.rgid = 1000; c.egid = 0; c.fsgid = 0;
	CHECK(credAccess(c, 0, 0, 0600, 4, false) == 0);    // effective/root
	CHECK(credAccess(c, 0, 0, 0600, 4, true)  == -13);  // real uid 1000, other has no read
}

TEST_CASE("credMaySticky: only file owner, dir owner, or root may delete") {
	Cred c; credInitRoot(c); c.euid = 1000; c.fsuid = 1000;
	CHECK(credMaySticky(c, /*dir*/0, /*file*/1000) == 0);   // file owner
	CHECK(credMaySticky(c, /*dir*/1000, /*file*/0) == 0);   // dir owner
	CHECK(credMaySticky(c, /*dir*/0, /*file*/0) == -1);     // neither -> EPERM
	Cred r; credInitRoot(r);
	CHECK(credMaySticky(r, 0, 0) == 0);                     // root
}
TEST_CASE("credMayChmod: owner or root") {
	Cred c; credInitRoot(c); c.euid = 1000; c.fsuid = 1000;
	CHECK(credMayChmod(c, 1000) == 0);
	CHECK(credMayChmod(c, 0) == -1);
	Cred r; credInitRoot(r); CHECK(credMayChmod(r, 0) == 0);
}
TEST_CASE("credMayChown: owner change root-only; group change needs ownership+membership") {
	Cred c; credInitRoot(c); c.euid = 1000; c.fsuid = 1000; c.ngroups = 1; c.groups[0] = 10;
	CHECK(credMayChown(c, 1000, -1, 10) == 0);     // own file, into a group I'm in
	CHECK(credMayChown(c, 1000, -1, 11) == -1);    // group I'm not in -> EPERM
	CHECK(credMayChown(c, 1000, 1001, -1) == -1);  // changing owner -> root only
	Cred r; credInitRoot(r); CHECK(credMayChown(r, 1000, 1001, 11) == 0);
}
TEST_CASE("credMayUtimes: toNow needs write; explicit needs owner/root") {
	Cred c; credInitRoot(c); c.euid = 1000; c.fsuid = 1000;
	CHECK(credMayUtimes(c, 1000, false) == 0);   // owner sets explicit
	CHECK(credMayUtimes(c, 0, false) == -1);     // not owner -> EPERM
}

TEST_CASE("credSetuid: root sets all four; non-root only among r/e/s") {
	Cred r; credInitRoot(r);
	CHECK(credSetuid(r, 1000) == 0);
	CHECK(r.ruid == 1000); CHECK(r.euid == 1000); CHECK(r.suid == 1000); CHECK(r.fsuid == 1000);
	Cred c; credInitRoot(c); c.ruid = 1000; c.euid = 1000; c.suid = 1000; c.fsuid = 1000;
	CHECK(credSetuid(c, 1000) == 0);   // identity ok
	CHECK(credSetuid(c, 0) == -1);     // can't become root
	Cred priv2; credInitRoot(priv2); priv2.ruid = 1000; priv2.euid = 0; priv2.suid = 0; priv2.fsuid = 0;
	CHECK(credSetuid(priv2, 1000) == 0);            // euid 0 -> privileged
	CHECK((priv2.ruid == 1000 && priv2.euid == 1000 && priv2.suid == 1000 && priv2.fsuid == 1000));
}
TEST_CASE("credSeteuid: may pick from {ruid,euid,suid}; fsuid follows") {
	Cred c; credInitRoot(c); c.ruid = 1000; c.euid = 0; c.suid = 0; c.fsuid = 0;
	CHECK(credSeteuid(c, 1000) == 0); CHECK(c.euid == 1000); CHECK(c.fsuid == 1000);
	CHECK(c.ruid == 1000); CHECK(c.suid == 0);   // ruid/suid unchanged
	CHECK(credSeteuid(c, 0) == 0);               // suid==0 still reachable
	Cred d; credInitRoot(d); d.ruid = 1000; d.euid = 1000; d.suid = 1000; d.fsuid = 1000;
	CHECK(credSeteuid(d, 0) == -1);              // 0 not in {1000} -> EPERM
}
TEST_CASE("credSetresuid: -1 leaves a field; unprivileged constrained to current r/e/s") {
	Cred c; credInitRoot(c); c.ruid = 1000; c.euid = 1000; c.suid = 1000; c.fsuid = 1000;
	CHECK(credSetresuid(c, -1, 1000, -1) == 0);
	CHECK(credSetresuid(c, 0, -1, -1) == -1);    // can't gain ruid 0
	Cred r; credInitRoot(r);
	CHECK(credSetresuid(r, 1, 2, 3) == 0);       // root sets anything
	CHECK((r.ruid == 1 && r.euid == 2 && r.suid == 3 && r.fsuid == 2));  // fsuid follows euid
}
TEST_CASE("credSetreuid: ruid in {ruid,euid}, euid in {ruid,euid,suid}; suid tracks") {
	Cred c; credInitRoot(c); c.ruid = 1000; c.euid = 1000; c.suid = 1000; c.fsuid = 1000;
	CHECK(credSetreuid(c, -1, 1000) == 0);
	Cred r; credInitRoot(r);
	CHECK(credSetreuid(r, 1000, 0) == 0);
	CHECK((r.ruid == 1000 && r.euid == 0)); CHECK(r.suid == 0);   // ruid changed -> suid=euid
}
TEST_CASE("credSetfsuid: returns previous, only to a permitted value") {
	Cred c; credInitRoot(c); c.ruid = 1000; c.euid = 1000; c.suid = 1000; c.fsuid = 1000;
	CHECK(credSetfsuid(c, 1000) == 1000);   // prev
	CHECK(c.fsuid == 1000);
	CHECK(credSetfsuid(c, 0) == 1000);      // not permitted -> unchanged, returns prev
	CHECK(c.fsuid == 1000);
}

TEST_CASE("credSetgroups: root only, bounded by NGROUPS_MAX") {
	Cred r; credInitRoot(r);
	unsigned g[3] = {10, 20, 30};
	CHECK(credSetgroups(r, g, 3) == 0);
	CHECK(r.ngroups == 3); CHECK(r.groups[2] == 30);
	Cred c; credInitRoot(c); c.euid = 1000;
	CHECK(credSetgroups(c, g, 3) == -1);                 // non-root EPERM
	unsigned big[NGROUPS_MAX + 1]; for (int i = 0; i < NGROUPS_MAX + 1; i++) big[i] = (unsigned) i;
	CHECK(credSetgroups(r, big, NGROUPS_MAX + 1) == -22);// EINVAL (too many)
}
TEST_CASE("credSetgid/segid/setregid/setresgid/setfsgid mirror the uid family") {
	Cred r; credInitRoot(r);
	CHECK(credSetgid(r, 1000) == 0);
	CHECK((r.rgid == 1000 && r.egid == 1000 && r.sgid == 1000 && r.fsgid == 1000));
	Cred c; credInitRoot(c); c.ruid = c.euid = c.suid = c.fsuid = 1000;   // unprivileged
	c.rgid = 1000; c.egid = 1000; c.sgid = 1000; c.fsgid = 1000;
	CHECK(credSetegid(c, 0) == -1);
	CHECK(credSetresgid(c, -1, 1000, -1) == 0);
	CHECK(credSetfsgid(c, 1000) == 1000);
}

TEST_CASE("credOnExec: setuid/setgid bits raise euid/egid; suid/sgid track") {
	Cred c; credInitRoot(c); c.ruid = c.euid = c.suid = c.fsuid = 1000;
	c.rgid = c.egid = c.sgid = c.fsgid = 1000;
	credOnExec(c, /*fileUid*/0, /*fileGid*/0, /*mode*/04755);   // setuid-root binary
	CHECK(c.euid == 0); CHECK(c.fsuid == 0); CHECK(c.suid == 0);// gained root euid; saved tracks
	CHECK(c.ruid == 1000);                                      // real uid unchanged
	Cred d; credInitRoot(d); d.ruid = d.euid = d.suid = 1000; d.rgid = d.egid = d.sgid = 1000;
	d.fsuid = 1000; d.fsgid = 1000;
	credOnExec(d, 0, 0, 0755);                                  // plain exec
	CHECK(d.euid == 1000); CHECK(d.suid == 1000);               // unchanged; suid:=euid
}

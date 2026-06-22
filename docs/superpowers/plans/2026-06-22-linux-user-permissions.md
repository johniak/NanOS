# Linux-Style User Permissions Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give NanOS a complete, enforced, Linux-faithful DAC model — real non-root accounts, mandatory console login, `su`/`sudo`/`passwd`, setuid/setgid/sticky semantics, the full saved-set-id credential family, supplementary groups, and `/etc/shadow` authentication.

**Architecture:** Credentials live in a host-tested `struct Cred` on the PCB (`Process::cred`, the Linux `task_struct->cred` analogue). All policy decisions are *pure functions* in `kernel/Cred.cpp`. The VFS enforces DAC (an `inode_permission()` analogue) using the caller's `Cred`, obtained via a `CredProvider` hook; FS drivers stay pure mechanism. `execve` honors setuid/setgid bits. Userland gets clean-room `crypt($6$)` + shadow/group parsing; tools are permissive ports (toybox 0BSD, sudo ISC).

**Tech Stack:** C++ freestanding kernel (`kernel/`, `fs/`), doctest host tests in the `nanos-test` Docker image (`make test`, ≥90% coverage gate), QEMU x86_64 (`make run64`), nanos-sdk cross-toolchain for userland ports.

**Spec:** `docs/superpowers/specs/2026-06-22-linux-user-permissions-design.md`

**Clean-room invariant:** our kernel + libc-glue code is original (semantics from POSIX/published specs, no copied Linux/glibc source). Only toybox/sudo are ports of permissively-licensed upstreams.

---

## Critical workflow facts (read before starting)

- **Everything builds in Docker; QEMU runs natively on macOS.** `make test` = host doctest suite in `nanos-test`. `make build` / `make run64` cross-compile + boot. Cross ARCH? `make clean` first. The user runs x86_64 (`make run64`, `make image64`).
- **Branch:** work on `feat/linux-user-permissions` (already created; the spec commit `6ff96ae` is there). Never commit to `master`. **No mention of Claude in any commit/PR.**
- **Two include lists:** kernel uses `-Iinclude` (freestanding `<string.h>`); host tests do not (libc). `kernel/Cred.*` and `fs/Vfs.*` must compile under BOTH — keep them MI (no `<arch/...>`, no x86). Run `make check-arch` after MI edits.
- **Host tests cannot `#include <cstdlib>`** (its C-linkage `malloc` clashes with `memory_manager.h`'s C++-linkage decl). Use the pattern in `tests/test_ext2.cpp` (doctest + `RamBlockDevice` + `loadFixture`).
- **Errno convention:** functions return *negated* errno. Existing: `EACCES 13`, `EROFS 30`, `ENOENT 2`, etc. in `kernel/Syscall.h`. We add `EPERM 1`. `permCheck`/`credAccess` return `-13` on denial; ownership/privilege violations return `-1` (`-EPERM`).
- **Existing creds:** `Syscalls::m_uid/m_euid/m_gid/m_egid` (`kernel/Syscall.h:126-127`), set to 0 in the ctor (`kernel/Syscall.cpp:51`), copied on fork (`:81-82`); `permCheck` (`:776-788`) is called only by `open()` (`:135-140`). These migrate to `Cred`.

---

## File structure (what each new/changed file owns)

**New:**
- `kernel/Cred.h` / `kernel/Cred.cpp` — the `Cred` struct + all pure permission & transition functions. MI, host-tested, the single source of DAC policy logic. **No I/O, no globals.**
- `tests/test_cred.cpp` — exhaustive unit tests for every `Cred` function/branch.
- `tests/test_vfs_perm.cpp` — VFS enforcement tests against an ext fixture with a stubbed `CredProvider`.
- `tests/test_crypt.cpp` — `$6$` crypt test vectors (built only when the libc-glue crypt unit is host-compilable; see Phase 4).
- `user/libc-glue/crypt.c` — clean-room SHA-512 `$6$` `crypt`/`crypt_r`.
- `user/libc-glue/grp_shadow.c` — `/etc/group` + `/etc/shadow` parsing, `getgrouplist`, `initgroups` (split out of `pwd_grp.c` to keep files focused).
- `config/group`, `config/shadow`, `config/sudoers` — seed account DB (alongside the existing `config/passwd`).

**Changed:**
- `kernel/Process.h` — add `Cred cred;` to `Process`; add `unsigned uid/gid/euid/egid/suid/sgid` + `int ngroups; unsigned groups[…]` snapshot to `ProcInfo`.
- `kernel/Syscall.h` / `kernel/Syscall.cpp` — drop `m_uid/m_euid/m_gid/m_egid`; hold `Cred* cred`; route credential syscalls through `Cred`; new credential methods.
- `kernel/SyscallNr.h` — add `setreuid/setregid/setresuid/getresuid/setresgid/getresgid/setfsuid/setfsgid/getgroups/setgroups` numbers (x86_64 + i386 + *32).
- `kernel/SyscallDispatch.cpp` — route the new numbers; thread the current process's `Cred*` into `Syscalls`.
- `fs/Vfs.h` / `fs/Vfs.cpp` — `CredProvider` hook + the policy layer (`maySearch/permission/mayCreate/mayDelete/mayChmod/mayChown/mayUtimes`) wrapping every public method; umask-on-create handled by the caller (`Syscalls`).
- `kernel/Exec.cpp` — `execve`/`execProgram` enforce X + apply setuid/setgid bits to `Process::cred`.
- `kernel/Kernel.cpp` — install the `CredProvider` (returns `ProcTable::current()->cred`) during boot.
- `fs/SynthFs.cpp` — render real `Uid:`/`Gid:`/`Groups:` lines in `/proc/<pid>/status` (currently hardcoded `0`, `:193-194`).
- `user/init.c` — exec `login` on the console instead of the shell directly.
- `user/libc-glue/pwd_grp.c` — read `/etc/passwd` (was `config/passwd`), tolerate `x` in field 2.
- `config/passwd` — field 2 becomes `x` (hash moves to `config/shadow`).
- `scripts/create-grub2-image.sh` + Makefile `_image` — install `config/{passwd,shadow,group,sudoers}`, the `/etc` symlinks, set modes, seed `root`+`jan`.
- `Makefile` — add `kernel/Cred.cpp` to `TEST_MODULES`/`COV_PATTERNS`; add toybox/sudo port targets.

---

## PHASE 1 — `Cred` core + credential syscalls (host-tested; no behavior change)

### Task 1: `Cred` struct + `credInitRoot` + `credInGroup`

**Files:**
- Create: `kernel/Cred.h`, `kernel/Cred.cpp`
- Create: `tests/test_cred.cpp`
- Modify: `Makefile:2123` (`TEST_MODULES`), and `COV_PATTERNS` (grep it: `make -n test 2>/dev/null` or search `COV_PATTERNS` in the Makefile)

- [ ] **Step 1: Write the failing test** — `tests/test_cred.cpp`:

```cpp
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
    c.egid = 5; c.fsgid = 5; c.ngroups = 2; c.groups[0] = 10; c.groups[1] = 10 /*wheel*/;
    c.groups[1] = 11;
    CHECK(credInGroup(c, 5));      // egid/fsgid
    CHECK(credInGroup(c, 10));     // supplementary
    CHECK(credInGroup(c, 11));
    CHECK_FALSE(credInGroup(c, 99));
}
```

- [ ] **Step 2: Run it; verify it fails to compile** — `make test` → FAIL (`Cred.h` not found / undefined symbols).

- [ ] **Step 3: Create `kernel/Cred.h`:**

```cpp
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
```

- [ ] **Step 4: Create `kernel/Cred.cpp` with the errno guards + the two functions:**

```cpp
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

} // namespace kernel
```

- [ ] **Step 5: Register the module** — in `Makefile`, append `kernel/Cred.cpp` to the `TEST_MODULES` list (line ~2123) and add `*/Cred.*` to `COV_PATTERNS`.

- [ ] **Step 6: Run `make test`; verify the two cases PASS** and the suite still green.

- [ ] **Step 7: Commit:**
```bash
git add kernel/Cred.h kernel/Cred.cpp tests/test_cred.cpp Makefile
git commit -m "feat(cred): Cred struct + credInitRoot/credInGroup (host-tested)"
```

### Task 2: `credAccess` (the core DAC decision)

**Files:** Modify `kernel/Cred.cpp`; Test `tests/test_cred.cpp`

- [ ] **Step 1: Add failing tests:**

```cpp
TEST_CASE("credAccess: owner/group/other selection by fsuid/fsgid") {
    Cred c; credInitRoot(c);
    c.ruid=c.euid=c.suid=c.fsuid=1000; c.rgid=c.egid=c.sgid=c.fsgid=1000;
    // file 0640 owned by 1000:1000 -> owner rw
    CHECK(credAccess(c, 1000, 1000, 0640, 4, false) == 0);     // read ok
    CHECK(credAccess(c, 1000, 1000, 0640, 2, false) == 0);     // write ok
    CHECK(credAccess(c, 1000, 1000, 0640, 1, false) == -13);   // no exec bit -> EACCES
    // file owned by 0:0 mode 0600 -> other has nothing
    CHECK(credAccess(c, 0, 0, 0600, 4, false) == -13);
    // group access: file 0:1000 mode 0040 -> group read via membership
    CHECK(credAccess(c, 0, 1000, 0040, 4, false) == 0);
}

TEST_CASE("credAccess: root bypasses r/w but needs an exec bit for x") {
    Cred c; credInitRoot(c);   // euid 0
    CHECK(credAccess(c, 1000, 1000, 0600, 6, false) == 0);   // rw on someone else's 0600
    CHECK(credAccess(c, 1000, 1000, 0600, 1, false) == -13); // x but no exec bits anywhere
    CHECK(credAccess(c, 1000, 1000, 0700, 1, false) == 0);   // an exec bit exists
}

TEST_CASE("credAccess: useReal uses ruid/rgid (access(2))") {
    Cred c; credInitRoot(c);
    c.ruid=1000; c.euid=0; c.fsuid=0; c.rgid=1000; c.egid=0; c.fsgid=0;
    // As euid 0 the fs-check passes rw; as REAL uid 1000 on a 0:0 0600 file it's denied.
    CHECK(credAccess(c, 0, 0, 0600, 4, false) == 0);    // effective/root
    CHECK(credAccess(c, 0, 0, 0600, 4, true)  == -13);  // real uid 1000, other has no read
}
```

- [ ] **Step 2: Run `make test`; verify FAIL** (`credAccess` undefined → link error).

- [ ] **Step 3: Implement in `kernel/Cred.cpp`:**

```cpp
int credAccess(const Cred& c, unsigned fileUid, unsigned fileGid, unsigned mode, int want, bool useReal) {
    if (want == 0) return 0;
    unsigned myUid = useReal ? c.ruid : c.fsuid;
    unsigned myGid = useReal ? c.rgid : c.fsgid;
    // Privileged: euid 0 always counts as root for the override below, even for access().
    if (c.euid == 0) {
        if ((want & 1) && (mode & 0111) == 0) return -EACCES;   // x still needs a bit somewhere
        return 0;
    }
    unsigned bits;
    if (fileUid == myUid)                              bits = (mode >> 6) & 7;   // owner
    else if (fileGid == myGid || credInGroupAs(c, fileGid, useReal)) bits = (mode >> 3) & 7;  // group
    else                                               bits = mode & 7;          // other
    return ((bits & (unsigned) want) == (unsigned) want) ? 0 : -EACCES;
}
```

  Add a small helper above it (group membership against the right gid set):

```cpp
// access(2) checks real gid + supplementary; effective checks egid/fsgid + supplementary.
static bool credInGroupAs(const Cred& c, unsigned gid, bool useReal) {
    if (useReal) { if (c.rgid == gid) return true; }
    else         { if (c.egid == gid || c.fsgid == gid) return true; }
    for (int i = 0; i < c.ngroups; i++) if (c.groups[i] == gid) return true;
    return false;
}
```

- [ ] **Step 4: Run `make test`; verify all credAccess cases PASS.**
- [ ] **Step 5: Commit:** `git commit -am "feat(cred): credAccess DAC decision (owner/group/other, root, real-vs-fs)"`

### Task 3: ownership / sticky / metadata rules

**Files:** Modify `kernel/Cred.cpp`; Test `tests/test_cred.cpp`

- [ ] **Step 1: Add failing tests:**

```cpp
TEST_CASE("credMaySticky: only file owner, dir owner, or root may delete") {
    Cred c; credInitRoot(c); c.euid=1000; c.fsuid=1000;
    CHECK(credMaySticky(c, /*dir*/0, /*file*/1000) == 0);   // file owner
    CHECK(credMaySticky(c, /*dir*/1000, /*file*/0) == 0);   // dir owner
    CHECK(credMaySticky(c, /*dir*/0, /*file*/0) == -1);     // neither -> EPERM
    Cred r; credInitRoot(r);
    CHECK(credMaySticky(r, 0, 0) == 0);                     // root
}
TEST_CASE("credMayChmod: owner or root") {
    Cred c; credInitRoot(c); c.euid=1000; c.fsuid=1000;
    CHECK(credMayChmod(c, 1000) == 0);
    CHECK(credMayChmod(c, 0) == -1);
    Cred r; credInitRoot(r); CHECK(credMayChmod(r, 0) == 0);
}
TEST_CASE("credMayChown: owner change root-only; group change needs ownership+membership") {
    Cred c; credInitRoot(c); c.euid=1000; c.fsuid=1000; c.ngroups=1; c.groups[0]=10;
    CHECK(credMayChown(c, 1000, -1, 10) == 0);     // own file, into a group I'm in
    CHECK(credMayChown(c, 1000, -1, 11) == -1);    // group I'm not in -> EPERM
    CHECK(credMayChown(c, 1000, 1001, -1) == -1);  // changing owner -> root only
    Cred r; credInitRoot(r); CHECK(credMayChown(r, 1000, 1001, 11) == 0);
}
TEST_CASE("credMayUtimes: toNow needs write; explicit needs owner/root") {
    Cred c; credInitRoot(c); c.euid=1000; c.fsuid=1000;
    CHECK(credMayUtimes(c, 1000, false) == 0);   // owner sets explicit
    CHECK(credMayUtimes(c, 0, false) == -1);     // not owner -> EPERM
}
```

- [ ] **Step 2: Run `make test`; verify FAIL.**
- [ ] **Step 3: Implement in `kernel/Cred.cpp`:**

```cpp
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
    if (c.fsuid != fileUid) return -EPERM;                           // must own the file
    if (newGid != -1 && !credInGroup(c, (unsigned) newGid)) return -EPERM;
    return 0;
}
int credMayUtimes(const Cred& c, unsigned fileUid, bool toNow) {
    if (c.euid == 0 || c.fsuid == fileUid) return 0;
    if (toNow) return 0;          // utimes(NULL): write perm is checked separately by the VFS
    return -EPERM;
}
```

- [ ] **Step 4: Run `make test`; PASS.**
- [ ] **Step 5: Commit:** `git commit -am "feat(cred): sticky/chmod/chown/utimes ownership rules"`

### Task 4: set*id transitions (uid family)

**Files:** Modify `kernel/Cred.cpp`; Test `tests/test_cred.cpp`

- [ ] **Step 1: Add failing tests** (cover privileged + unprivileged + saved-id + fsuid-follow):

```cpp
TEST_CASE("credSetuid: root sets all four; non-root only among r/e/s") {
    Cred r; credInitRoot(r);
    CHECK(credSetuid(r, 1000) == 0);
    CHECK(r.ruid==1000); CHECK(r.euid==1000); CHECK(r.suid==1000); CHECK(r.fsuid==1000);
    Cred c; credInitRoot(c); c.ruid=1000; c.euid=1000; c.suid=1000; c.fsuid=1000;
    CHECK(credSetuid(c, 1000) == 0);   // identity ok
    CHECK(credSetuid(c, 0) == -1);     // can't become root
    Cred priv; credInitRoot(priv); priv.ruid=1000; priv.euid=0; priv.suid=0; priv.fsuid=0;
    CHECK(credSetuid(priv, 1000) == 0);            // euid 0 -> privileged
    CHECK(priv.ruid==1000 && priv.euid==1000 && priv.suid==1000 && priv.fsuid==1000);
}
TEST_CASE("credSeteuid: may pick from {ruid,euid,suid}; fsuid follows") {
    Cred c; credInitRoot(c); c.ruid=1000; c.euid=0; c.suid=0; c.fsuid=0;
    CHECK(credSeteuid(c, 1000) == 0); CHECK(c.euid==1000); CHECK(c.fsuid==1000);
    CHECK(c.ruid==1000); CHECK(c.suid==0);   // ruid/suid unchanged
    CHECK(credSeteuid(c, 0) == 0);           // suid==0 still reachable
    Cred d; credInitRoot(d); d.ruid=1000; d.euid=1000; d.suid=1000; d.fsuid=1000;
    CHECK(credSeteuid(d, 0) == -1);          // 0 not in {1000} -> EPERM
}
TEST_CASE("credSetresuid: -1 leaves a field; unprivileged constrained to current r/e/s") {
    Cred c; credInitRoot(c); c.ruid=1000; c.euid=1000; c.suid=1000; c.fsuid=1000;
    CHECK(credSetresuid(c, -1, 1000, -1) == 0);
    CHECK(credSetresuid(c, 0, -1, -1) == -1);    // can't gain ruid 0
    Cred r; credInitRoot(r);
    CHECK(credSetresuid(r, 1, 2, 3) == 0);       // root sets anything
    CHECK(r.ruid==1 && r.euid==2 && r.suid==3 && r.fsuid==2);  // fsuid follows euid
}
TEST_CASE("credSetreuid: ruid in {ruid,euid}, euid in {ruid,euid,suid}; suid tracks") {
    Cred c; credInitRoot(c); c.ruid=1000; c.euid=1000; c.suid=1000; c.fsuid=1000;
    CHECK(credSetreuid(c, -1, 1000) == 0);
    Cred r; credInitRoot(r);
    CHECK(credSetreuid(r, 1000, 0) == 0);
    CHECK(r.ruid==1000 && r.euid==0); CHECK(r.suid==0);   // ruid changed -> suid=euid
}
TEST_CASE("credSetfsuid: returns previous, only to a permitted value") {
    Cred c; credInitRoot(c); c.ruid=1000; c.euid=1000; c.suid=1000; c.fsuid=1000;
    CHECK(credSetfsuid(c, 1000) == 1000);   // prev
    CHECK(c.fsuid == 1000);
    CHECK(credSetfsuid(c, 0) == 1000);      // not permitted -> unchanged, returns prev
    CHECK(c.fsuid == 1000);
}
```

- [ ] **Step 2: Run `make test`; verify FAIL.**
- [ ] **Step 3: Implement in `kernel/Cred.cpp`:**

```cpp
static bool priv(const Cred& c) { return c.euid == 0; }
static bool oneOfRES_u(const Cred& c, unsigned v) { return v==c.ruid || v==c.euid || v==c.suid; }

int credSetuid(Cred& c, unsigned uid) {
    if (priv(c)) { c.ruid=c.euid=c.suid=c.fsuid=uid; return 0; }
    if (uid == c.ruid || uid == c.suid) { c.euid=c.fsuid=uid; return 0; }
    return -EPERM;
}
int credSeteuid(Cred& c, unsigned euid) {
    if (priv(c) || oneOfRES_u(c, euid)) { c.euid=c.fsuid=euid; return 0; }
    return -EPERM;
}
int credSetreuid(Cred& c, int ruid, int euid) {
    unsigned nr = (ruid==-1)? c.ruid : (unsigned)ruid;
    unsigned ne = (euid==-1)? c.euid : (unsigned)euid;
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
    unsigned nr=(r==-1)?c.ruid:(unsigned)r, ne=(e==-1)?c.euid:(unsigned)e, ns=(s==-1)?c.suid:(unsigned)s;
    if (!priv(c)) {
        if (r != -1 && !oneOfRES_u(c, nr)) return -EPERM;
        if (e != -1 && !oneOfRES_u(c, ne)) return -EPERM;
        if (s != -1 && !oneOfRES_u(c, ns)) return -EPERM;
    }
    c.ruid=nr; c.euid=ne; c.suid=ns; c.fsuid=ne;   // fsuid follows euid
    return 0;
}
unsigned credSetfsuid(Cred& c, unsigned fsuid) {
    unsigned prev = c.fsuid;
    if (priv(c) || fsuid==c.ruid || fsuid==c.euid || fsuid==c.suid || fsuid==c.fsuid)
        c.fsuid = fsuid;
    return prev;
}
```

- [ ] **Step 4: Run `make test`; PASS.**
- [ ] **Step 5: Commit:** `git commit -am "feat(cred): uid transition family (setuid/seteuid/setreuid/setresuid/setfsuid)"`

### Task 5: set*id transitions (gid family) + setgroups

**Files:** Modify `kernel/Cred.cpp`; Test `tests/test_cred.cpp`

- [ ] **Step 1: Add failing tests** mirroring Task 4 for gids, plus:

```cpp
TEST_CASE("credSetgroups: root only, bounded by NGROUPS_MAX") {
    Cred r; credInitRoot(r);
    unsigned g[3] = {10, 20, 30};
    CHECK(credSetgroups(r, g, 3) == 0);
    CHECK(r.ngroups == 3); CHECK(r.groups[2] == 30);
    Cred c; credInitRoot(c); c.euid=1000;
    CHECK(credSetgroups(c, g, 3) == -1);                 // non-root EPERM
    unsigned big[NGROUPS_MAX + 1]; for (int i=0;i<NGROUPS_MAX+1;i++) big[i]=i;
    CHECK(credSetgroups(r, big, NGROUPS_MAX + 1) == -22);// EINVAL (too many)
}
TEST_CASE("credSetgid/segid/setregid/setresgid/setfsgid mirror the uid family") {
    Cred r; credInitRoot(r);
    CHECK(credSetgid(r, 1000) == 0); CHECK(r.rgid==1000 && r.egid==1000 && r.sgid==1000 && r.fsgid==1000);
    Cred c; credInitRoot(c); c.rgid=1000; c.egid=1000; c.sgid=1000; c.fsgid=1000;
    CHECK(credSetegid(c, 0) == -1);
    CHECK(credSetresgid(c, -1, 1000, -1) == 0);
    CHECK(credSetfsgid(c, 1000) == 1000);
}
```

- [ ] **Step 2: Run `make test`; verify FAIL.**
- [ ] **Step 3: Implement the gid mirrors + `credSetgroups` in `kernel/Cred.cpp`** (identical structure to the uid family, swapping r/e/s/fs-gid; `priv` is still `euid==0`):

```cpp
static bool oneOfRES_g(const Cred& c, unsigned v) { return v==c.rgid || v==c.egid || v==c.sgid; }
int credSetgid(Cred& c, unsigned gid) {
    if (priv(c)) { c.rgid=c.egid=c.sgid=c.fsgid=gid; return 0; }
    if (gid==c.rgid || gid==c.sgid) { c.egid=c.fsgid=gid; return 0; }
    return -EPERM;
}
int credSetegid(Cred& c, unsigned egid) {
    if (priv(c) || oneOfRES_g(c, egid)) { c.egid=c.fsgid=egid; return 0; }
    return -EPERM;
}
int credSetregid(Cred& c, int rgid, int egid) {
    unsigned nr=(rgid==-1)?c.rgid:(unsigned)rgid, ne=(egid==-1)?c.egid:(unsigned)egid;
    if (!priv(c)) {
        if (rgid != -1 && nr != c.rgid && nr != c.egid) return -EPERM;
        if (egid != -1 && ne != c.rgid && ne != c.egid && ne != c.sgid) return -EPERM;
    }
    bool rch=(rgid!=-1 && nr!=c.rgid), enr=(egid!=-1 && ne!=c.rgid);
    c.rgid=nr; c.egid=ne; c.fsgid=ne; if (rch||enr) c.sgid=c.egid;
    return 0;
}
int credSetresgid(Cred& c, int r, int e, int s) {
    unsigned nr=(r==-1)?c.rgid:(unsigned)r, ne=(e==-1)?c.egid:(unsigned)e, ns=(s==-1)?c.sgid:(unsigned)s;
    if (!priv(c)) {
        if (r!=-1 && !oneOfRES_g(c,nr)) return -EPERM;
        if (e!=-1 && !oneOfRES_g(c,ne)) return -EPERM;
        if (s!=-1 && !oneOfRES_g(c,ns)) return -EPERM;
    }
    c.rgid=nr; c.egid=ne; c.sgid=ns; c.fsgid=ne; return 0;
}
unsigned credSetfsgid(Cred& c, unsigned fsgid) {
    unsigned prev=c.fsgid;
    if (priv(c)||fsgid==c.rgid||fsgid==c.egid||fsgid==c.sgid||fsgid==c.fsgid) c.fsgid=fsgid;
    return prev;
}
int credSetgroups(Cred& c, const unsigned* g, int n) {
    if (!priv(c)) return -EPERM;
    if (n < 0 || n > NGROUPS_MAX) return -EINVAL;
    c.ngroups = n;
    for (int i = 0; i < n; i++) c.groups[i] = g[i];
    return 0;
}
```

- [ ] **Step 4: Run `make test`; PASS.**
- [ ] **Step 5: Commit:** `git commit -am "feat(cred): gid transition family + setgroups"`

### Task 6: migrate `Syscalls` credentials onto `Cred*`

**Files:** Modify `kernel/Syscall.h`, `kernel/Syscall.cpp`; Test `tests/test_syscall.cpp` (or `test_cred.cpp` if Syscalls tests need a Cred fixture)

- [ ] **Step 1: Write a failing test** in `tests/test_syscall.cpp` (find the existing Syscalls construction pattern there) that constructs a `Syscalls` with an injected `Cred` and checks `getuid()` reflects it:

```cpp
// (Add near the other Syscalls tests; the harness builds a Vfs over a RamFs/SynthFs already.)
TEST_CASE("Syscalls reads identity from the injected Cred") {
    Cred cred; credInitRoot(cred); cred.ruid = 1000; cred.euid = 1000;
    Vfs vfs;  /* ...existing minimal mount used by the other tests... */
    Syscalls s(&vfs, /*consoleWrite*/ nullptr);
    s.setCred(&cred);
    CHECK(s.getuid() == 1000);
    CHECK(s.geteuid() == 1000);
}
```

- [ ] **Step 2: Run `make test`; verify FAIL** (`setCred` undefined).

- [ ] **Step 3: Edit `kernel/Syscall.h`:** include `"Cred.h"`; **remove** `unsigned m_uid, m_gid; unsigned m_euid, m_egid;` (`:126-127`); add `Cred* cred;` (default `nullptr`) plus `Cred ownCred;` (a fallback the ctor points `cred` at, so a standalone `Syscalls` — e.g. host tests, early boot — still works without a Process). Add public `void setCred(Cred* c) { cred = c ? c : &ownCred; }` and change every `m_uid/m_euid/m_gid/m_egid` reference to `cred->ruid/euid/rgid/egid` accordingly. Declare the new credential methods (Task 7).

- [ ] **Step 4: Edit `kernel/Syscall.cpp`:**
  - ctor (`:44-70`): replace `this->m_uid = ... = 0;` (`:51`) with `credInitRoot(ownCred); cred = &ownCred;`.
  - copy-ctor (`:75-96`): replace `m_uid=o.m_uid; ...` (`:81-82`) with `ownCred = *o.cred; cred = &ownCred;` (the child gets its own copy; `SyscallDispatch` re-points it at the new `Process::cred` after fork — see Task 11).
  - `getuid/geteuid/getgid/getegid` (`:757-760`): return `cred->ruid/euid/rgid/egid`.
  - `permCheck` (`:776-788`): reimplement as a thin forward — `return credAccess(... )` using `*cred`. Since `permCheck` takes a `FileStat`, call `credAccess(*cred, st.uid, st.gid, st.mode, want, /*useReal*/false);`.
  - `setuid/setgid` (`:763-772`): forward to `credSetuid(*cred, uid)` / `credSetgid(*cred, gid)` (returns 0/-EPERM).

- [ ] **Step 5: Run `make test`; verify the new case + all existing Syscalls/open tests PASS** (behavior is identical: everyone still root unless a test injects otherwise). Run `make check-arch` (Cred is MI).

- [ ] **Step 6: Commit:** `git commit -am "refactor(syscall): credentials live in Cred*; permCheck forwards to credAccess"`

### Task 7: credential syscalls + numbers + dispatch

**Files:** Modify `kernel/Syscall.h`, `kernel/Syscall.cpp`, `kernel/SyscallNr.h`, `kernel/SyscallDispatch.cpp`; Test `tests/test_syscall.cpp`

- [ ] **Step 1: Add failing tests** exercising the new `Syscalls` methods against an injected non-root `Cred`:

```cpp
TEST_CASE("Syscalls credential setters enforce POSIX rules via Cred") {
    Cred cred; credInitRoot(cred); cred.ruid=cred.euid=cred.suid=1000;
    cred.rgid=cred.egid=cred.sgid=1000; cred.fsuid=1000; cred.fsgid=1000;
    Vfs vfs; Syscalls s(&vfs, nullptr); s.setCred(&cred);
    CHECK(s.seteuid(0) == -1);                    // can't gain root
    CHECK(s.setresuid(-1, 1000, -1) == 0);
    int ru, eu, su; CHECK(s.getresuid(&ru,&eu,&su) == 0);
    CHECK(ru==1000 && eu==1000 && su==1000);
    unsigned g[2] = {10,20};
    CHECK(s.setgroups(2, g) == -1);               // non-root
    cred.euid = 0;                                 // become root
    CHECK(s.setgroups(2, g) == 0);
    unsigned out[8]; CHECK(s.getgroups(8, out) == 2);
}
```

- [ ] **Step 2: Run `make test`; verify FAIL.**

- [ ] **Step 3: Add the new `Syscalls` methods** (declarations in `kernel/Syscall.h`, bodies in `kernel/Syscall.cpp`), each a thin forward to `Cred` (clamp `unsigned` args from the ABI; `-1`/`0xFFFFFFFF` means "unchanged"):

```cpp
int Syscalls::getresuid(int* r,int* e,int* s){ *r=cred->ruid; *e=cred->euid; *s=cred->suid; return 0; }
int Syscalls::getresgid(int* r,int* e,int* s){ *r=cred->rgid; *e=cred->egid; *s=cred->sgid; return 0; }
int Syscalls::seteuid(int e){ return credSeteuid(*cred,(unsigned)e); }
int Syscalls::setegid(int e){ return credSetegid(*cred,(unsigned)e); }
int Syscalls::setreuid(int r,int e){ return credSetreuid(*cred, r, e); }   // -1 passthrough
int Syscalls::setregid(int r,int e){ return credSetregid(*cred, r, e); }
int Syscalls::setresuid(int r,int e,int s){ return credSetresuid(*cred,r,e,s); }
int Syscalls::setresgid(int r,int e,int s){ return credSetresgid(*cred,r,e,s); }
int Syscalls::setfsuid(int u){ return (int) credSetfsuid(*cred,(unsigned)u); }   // returns prev fsuid
int Syscalls::setfsgid(int g){ return (int) credSetfsgid(*cred,(unsigned)g); }
int Syscalls::getgroups(int size, unsigned* list){
    if (size == 0) return cred->ngroups;
    if (size < cred->ngroups) return -22;          // -EINVAL
    for (int i=0;i<cred->ngroups;i++) list[i]=cred->groups[i];
    return cred->ngroups;
}
int Syscalls::setgroups(int n, const unsigned* list){ return credSetgroups(*cred, list, n); }
```

- [ ] **Step 4: Add the numbers to `kernel/SyscallNr.h`.** The x86_64 block (lines 11–152) uses **real Linux x86_64 numbers**; the values below are the genuine ones and are **verified free** in that block (113–123 are unused there — the `118/119/120` you may see in the file belong to the *i386* block: fsync/sigreturn/clone). Add to the x86_64 block (near `:92-108`):

```c
#define SYS_setreuid   113
#define SYS_setregid   114
#define SYS_getgroups  115
#define SYS_setgroups  116
#define SYS_setresuid  117
#define SYS_getresuid  118
#define SYS_setresgid  119
#define SYS_getresgid  120
#define SYS_setfsuid   122
#define SYS_setfsgid   123
```
  **i386 block (lines ≥154):** i686 is **FROZEN** — no new i386 userland is built, so these names exist only so the *shared* `SyscallDispatch` case labels compile. Before adding, grep the i386 block for each candidate number; NanOS reassigns several (e.g. i386 `fsync=118`, `sigreturn=119`, `clone=120`). Use the real Linux i386 number **only if free in that block**; otherwise use a NanOS-private sentinel `≥1001` (the exact pattern the file already uses for x86_64 legacy calls, `:138-150`). The dispatch never receives these from a real i386 program, so the value only needs to be unique. The real i386 numbers (use where free, else sentinel):
```c
#define SYS_setreuid   70
#define SYS_setregid   71
#define SYS_getgroups  80
#define SYS_setgroups  81
#define SYS_setfsuid   138
#define SYS_setfsgid   139
#define SYS_setresuid  164
#define SYS_getresuid  165
#define SYS_setresgid  170
#define SYS_getresgid  171
#define SYS_setreuid32 203
#define SYS_setregid32 204
#define SYS_getgroups32 205
#define SYS_setgroups32 206
#define SYS_setresuid32 208
#define SYS_getresuid32 209
#define SYS_setresgid32 210
#define SYS_getresgid32 211
#define SYS_setfsuid32 215
#define SYS_setfsgid32 216
```

- [ ] **Step 5: Route them in `kernel/SyscallDispatch.cpp`** (next to the existing `SYS_getuid` cases at `:737`). `a0/a1/a2` are the arg registers already used elsewhere; for the `getres*`/`getgroups` ones the args are userland pointers (cast like other pointer syscalls in the file):

```cpp
case SYS_seteuid:                       ret = g_sys->seteuid((int)a0); break;
case SYS_setegid:                       ret = g_sys->setegid((int)a0); break;
case SYS_setreuid: case SYS_setreuid32: ret = g_sys->setreuid((int)a0,(int)a1); break;
case SYS_setregid: case SYS_setregid32: ret = g_sys->setregid((int)a0,(int)a1); break;
case SYS_setresuid: case SYS_setresuid32: ret = g_sys->setresuid((int)a0,(int)a1,(int)a2); break;
case SYS_setresgid: case SYS_setresgid32: ret = g_sys->setresgid((int)a0,(int)a1,(int)a2); break;
case SYS_getresuid: case SYS_getresuid32: ret = g_sys->getresuid((int*)a0,(int*)a1,(int*)a2); break;
case SYS_getresgid: case SYS_getresgid32: ret = g_sys->getresgid((int*)a0,(int*)a1,(int*)a2); break;
case SYS_setfsuid: case SYS_setfsuid32: ret = g_sys->setfsuid((int)a0); break;
case SYS_setfsgid: case SYS_setfsgid32: ret = g_sys->setfsgid((int)a0); break;
case SYS_getgroups: case SYS_getgroups32: ret = g_sys->getgroups((int)a0,(unsigned*)a1); break;
case SYS_setgroups: case SYS_setgroups32: ret = g_sys->setgroups((int)a0,(const unsigned*)a1); break;
```
  Note: `seteuid`/`setegid` have no dedicated x86_64 number — define them only if NanOS userland calls them directly; otherwise the libc wrapper (Phase 4) routes via `setresuid`. If you keep them, add i386-only `#define SYS_seteuid 138`-style is wrong (138 is setfsuid); instead implement `seteuid` purely as a libc wrapper. **Decision: drop the `SYS_seteuid/SYS_setegid` cases above; implement them as libc wrappers over setresuid in Phase 4.** (Keeps the kernel table matching real Linux.)

- [ ] **Step 6: Run `make test`; PASS. `make build` (x86_64) to confirm the dispatch compiles.**
- [ ] **Step 7: Commit:** `git commit -am "feat(syscall): full credential syscall family (setres*/setre*/setfs*/get|setgroups)"`

---

## PHASE 2 — VFS enforcement layer + CredProvider + umask fixes

### Task 8: `CredProvider` hook + `Vfs::maySearch` (path-walk) + `Vfs::permission`

**Files:** Modify `fs/Vfs.h`, `fs/Vfs.cpp`; Create `tests/test_vfs_perm.cpp`

- [ ] **Step 1: Write failing tests** in `tests/test_vfs_perm.cpp`. Use the ext fixture (`loadFixture` pattern from `tests/test_ext2.cpp`) **or** a `RamFs` so the test can `create`/`chmod`/`mkdir`. Install a stubbed `CredProvider` returning a test `Cred`:

```cpp
#include "doctest.h"
#include "Vfs.h"
#include "RamFs.h"
#include "Cred.h"
using namespace kernel;

static Cred g_testCred;
static const Cred* testProvider() { return &g_testCred; }

TEST_CASE("Vfs denies read without permission and allows it with") {
    RamFs* rfs = new RamFs();
    Vfs vfs; vfs.mount("/", rfs);
    vfs.setCredProvider(testProvider);
    credInitRoot(g_testCred);                       // root: create the fixture
    vfs.create("/secret", 0600);
    vfs.chown("/secret", 0, 0);
    // become an unprivileged user
    credInitRoot(g_testCred); g_testCred.ruid=g_testCred.euid=g_testCred.fsuid=1000;
    g_testCred.rgid=g_testCred.egid=g_testCred.fsgid=1000;
    char buf[4];
    CHECK(vfs.read("/secret", 1, 0, buf) == -13);   // EACCES: other has no read on 0600 root file
    // root reads it fine
    credInitRoot(g_testCred);
    CHECK(vfs.read("/secret", 0, 0, buf) >= 0);
}

TEST_CASE("Vfs path-walk requires search (x) on every ancestor directory") {
    RamFs* rfs = new RamFs();
    Vfs vfs; vfs.mount("/", rfs); vfs.setCredProvider(testProvider);
    credInitRoot(g_testCred);
    vfs.mkdir("/dir", 0700); vfs.chown("/dir", 0, 0);   // root-only dir, no x for others
    vfs.create("/dir/f", 0644);
    credInitRoot(g_testCred); g_testCred.ruid=g_testCred.euid=g_testCred.fsuid=1000;
    char b[4];
    CHECK(vfs.read("/dir/f", 1, 0, b) == -13);          // can't search /dir
}
```

- [ ] **Step 2: Run `make test`; verify FAIL** (`setCredProvider` undefined; reads currently succeed).

- [ ] **Step 3: Edit `fs/Vfs.h`:** include `"Cred.h"`; add the typedef + a member + setter, and private policy helpers:

```cpp
typedef const Cred* (*CredProvider)();
// inside class Vfs (private):
CredProvider credProvider = nullptr;
const Cred* caller();                                  // credProvider() or nullptr
int maySearch(String path);                            // x on every ancestor dir; 0 / -EACCES
int permission(String path, int want);                 // walk + want on the final object
int mayCreate(String path);                            // walk + W on parent dir
int mayDelete(String path);                            // walk + W on parent + sticky
// public:
void setCredProvider(CredProvider p) { credProvider = p; }
```

- [ ] **Step 4: Implement the helpers in `fs/Vfs.cpp`.** `caller()` returns `credProvider ? credProvider() : nullptr`; **a null caller bypasses all checks** (early boot / kernel context = root-equivalent). Use the existing internal `stat`/`lstat`. Walk each `/`-separated prefix:

```cpp
const Cred* Vfs::caller() { return credProvider ? credProvider() : nullptr; }

int Vfs::maySearch(String path) {
    const Cred* c = caller(); if (!c) return 0;
    // Check x on every ANCESTOR directory (not the final component).
    const char* s = path.c_str();
    char acc[256]; int n = 0; acc[n++] = '/';
    for (int i = 1; s[i]; i++) {
        if (s[i] == '/') {
            acc[n] = 0;
            FileStat st;
            if (statNoCheck(String(acc), st) < 0) return -2;     // -ENOENT (statNoCheck: no recursion)
            int pc = credAccess(*c, st.uid, st.gid, st.mode, 1 /*x*/, false);
            if (pc < 0) return pc;
        }
        if (n < 255) acc[n++] = s[i];
    }
    return 0;
}

int Vfs::permission(String path, int want) {
    int sc = maySearch(path); if (sc < 0) return sc;
    const Cred* c = caller(); if (!c) return 0;
    FileStat st; if (statNoCheck(path, st) < 0) return -2;
    return credAccess(*c, st.uid, st.gid, st.mode, want, false);
}
```
  (`mayCreate`/`mayDelete` come in Task 9.)

- [ ] **Step 5: Gate `Vfs::read` on `permission(path, 4)`** at the top of `fs/Vfs.cpp::read` (return the error if `< 0`), and likewise make `Vfs::stat`/`lstat`/`readlink`/`readdir` call `maySearch` first (they need only ancestor search, no object perm). **Careful:** `permission()`/`maySearch()` themselves call `stat`, which would recurse if `stat` gates on `maySearch`. Resolve by having the policy helpers call a private `statNoCheck()` (rename the current FS-delegating body) while the public `stat()` does `maySearch` then `statNoCheck`.

- [ ] **Step 6: Run `make test`; the two new cases PASS; existing tests still green** (existing tests install no provider → bypass). `make check-arch` clean.
- [ ] **Step 7: Commit:** `git commit -am "feat(vfs): CredProvider + path-walk search + read permission (inode_permission analogue)"`

### Task 9: enforce write/create/delete/rename/chmod/chown/utimes/truncate + umask fixes

**Files:** Modify `fs/Vfs.cpp`, `fs/Vfs.h`, `kernel/Syscall.cpp`; Test `tests/test_vfs_perm.cpp`

- [ ] **Step 1: Add failing tests** for: `write` needs W; `create`/`mkdir`/`symlink`/`mknod` need W on parent; `unlink`/`rmdir` need W on parent; **sticky** dir lets only the owner delete; `rename` checks both parents; `chmod` non-owner → `-1`; `chown` owner→root-only; `truncate` needs W; `access(useReal)`; `mkdir`/`create` apply umask. Example sticky case:

```cpp
TEST_CASE("Vfs sticky directory: non-owner cannot unlink another's file") {
    RamFs* rfs = new RamFs(); Vfs vfs; vfs.mount("/", rfs); vfs.setCredProvider(testProvider);
    credInitRoot(g_testCred);
    vfs.mkdir("/tmp", 01777);                       // sticky, world-writable
    vfs.create("/tmp/root_file", 0644); vfs.chown("/tmp/root_file", 0, 0);
    credInitRoot(g_testCred); g_testCred.ruid=g_testCred.euid=g_testCred.fsuid=1000;
    g_testCred.rgid=g_testCred.egid=g_testCred.fsgid=1000;
    CHECK(vfs.unlink("/tmp/root_file") == -1);      // EPERM: sticky, not the owner
    vfs.create("/tmp/mine", 0644); vfs.chown("/tmp/mine", 1000, 1000);
    CHECK(vfs.unlink("/tmp/mine") == 0);            // own file in sticky dir
}
```

- [ ] **Step 2: Run `make test`; verify FAIL.**

- [ ] **Step 3: Implement `mayCreate`/`mayDelete` in `fs/Vfs.cpp`** and gate each public mutator. `mayCreate(path)` = `maySearch(path)` + W on `dirname(path)`. `mayDelete(path)` = `maySearch` + W on parent + (parent has `S_ISVTX 01000` ⇒ `credMaySticky(parentUid, targetUid)`):

```cpp
int Vfs::mayCreate(String path) {
    int sc = maySearch(path); if (sc < 0) return sc;
    const Cred* c = caller(); if (!c) return 0;
    FileStat pd; if (statNoCheck(parentOf(path), pd) < 0) return -2;
    return credAccess(*c, pd.uid, pd.gid, pd.mode, 2 /*w*/, false);
}
int Vfs::mayDelete(String path) {
    int mc = mayCreate(path); if (mc < 0) return mc;        // W on parent (+ search)
    const Cred* c = caller(); if (!c) return 0;
    FileStat pd; statNoCheck(parentOf(path), pd);
    if (pd.mode & 01000) {                                   // sticky parent
        FileStat ts; if (statNoCheck(path, ts) < 0) return -2;
        return credMaySticky(*c, pd.uid, ts.uid);
    }
    return 0;
}
```
  Add a tiny `parentOf(String)` helper (strip the last `/component`; root's parent is `/`).
  Gate: `write`→`permission(path,2)`; `create/mkdir/mknod/symlink`→`mayCreate`; `link(new)`→`mayCreate(newpath)`; `unlink/rmdir`→`mayDelete`; `rename(old,new)`→`mayDelete(old)` then `mayCreate(new)`; `truncate`→`permission(path,2)`; `chmod`→`maySearch`+`credMayChmod`; `chown/lchown`→`maySearch`+`credMayChown`; `utimes`→`maySearch`+`credMayUtimes` (toNow = caller passed "now"; see below).

- [ ] **Step 4: Fix the umask bugs in `kernel/Syscall.cpp`** (the VFS doesn't know umask; the *caller* applies it): in `open()` (`:124`) replace the hardcoded `vfs->create(path, 0644)` with `vfs->create(path, (0666 & ~cred? ... ))` — actually umask lives in `Syscalls::m_umask`; use it: `vfs->create(path, 0666 & ~m_umask)`. In `mkdir()` (`:547`) pass `(unsigned) mode & ~m_umask`. (`mkdirat` already masks.) New files/dirs get owner = caller's `fsuid` and group per parent's setgid bit — implement that in the **FS create path** is wrong (FS is mechanism); instead, after a successful `vfs->create`, the VFS sets ownership from `caller()` in its create wrapper: on create, `chownNoCheck(path, c->fsuid, parentSetgid ? parentGid : c->fsgid)`. Add a `chownNoCheck`/`chmodNoCheck` private path used by the policy layer to avoid re-checking.

- [ ] **Step 5: Run `make test`; PASS. `make check-arch` clean.**
- [ ] **Step 6: Commit:** `git commit -am "feat(vfs): enforce write/create/delete/rename/chmod/chown/truncate + sticky + umask + new-file ownership"`

### Task 10: install the `CredProvider` in the kernel

**Files:** Modify `kernel/Kernel.cpp`, `kernel/SyscallDispatch.cpp`

- [ ] **Step 1:** Add a free function (in `SyscallDispatch.cpp` where `ProcTable` is available) `static const Cred* currentCredProvider() { Process* p = ProcTable::current(); return p ? &p->cred : nullptr; }`.
- [ ] **Step 2:** In `kernel/Kernel.cpp`, right after the `Vfs` is constructed and mounts are set up (search `new Vfs` / `vfs->mount("/"`), call `vfs->setCredProvider(currentCredProvider);` (expose it via a small `installCredProvider(Vfs*)` in `SyscallDispatch.cpp` to keep `Kernel.cpp` clean, mirroring `installSyscalls(Vfs*)`).
- [ ] **Step 3: Build + boot** `make run64` headless; confirm boot still reaches the shell with no `v=0d` GP faults (everything is root, so enforcement is a no-op functionally, but the walk now runs). Verify via the QEMU screendump pattern in CLAUDE.md.
- [ ] **Step 4: Commit:** `git commit -am "feat(kernel): install CredProvider so the VFS enforces with the running process's Cred"`

---

## PHASE 3 — setuid/setgid at exec, fork cred wiring, /proc

### Task 11: fork copies `Process::cred`; dispatch re-points `Syscalls::cred`

**Files:** Modify `kernel/Exec.cpp` (fork path ~`:218-256`), `kernel/Process.h`

- [ ] **Step 1:** Add `Cred cred;` to `struct Process` (`kernel/Process.h:74`), initialized to root in `ProcTable::alloc` (or `init`).
- [ ] **Step 2:** In the fork path (`kernel/Exec.cpp`, `child = ProcTable::alloc(...)`), after creating the child's `Syscalls` copy: `child->cred = parent->cred;` then `child->sys->setCred(&child->cred);` so the child's `Syscalls` points at the child's `Process::cred` (not the parent's, not the copy-ctor's `ownCred`). Likewise for the initial PID-1 setup and any place a `Process` gets a fresh `Syscalls`, call `setCred(&proc->cred)`.
- [ ] **Step 3: Test** (host, in `tests/test_syscall.cpp` if the fork bookkeeping is reachable, else verify in QEMU Phase 8): a forked child mutating its `cred` must not change the parent's. Add a focused host test if `Process`/fork bookkeeping is host-constructible; otherwise note it's covered by the QEMU acceptance (`su` in a subshell doesn't change the parent shell's id).
- [ ] **Step 4: Build + boot; commit:** `git commit -am "feat(proc): Process owns Cred; fork copies it; Syscalls points at it"`

### Task 12: honor setuid/setgid bits at `execve`

**Files:** Modify `kernel/Exec.cpp` (`execve` ~`:124`, `execProgram` ~`:70`); Test: QEMU (Phase 8) + a host unit on a pure helper

- [ ] **Step 1: Extract a pure helper** `void credOnExec(Cred& c, unsigned fileUid, unsigned fileGid, unsigned mode)` into `kernel/Cred.cpp` and unit-test it in `tests/test_cred.cpp`:

```cpp
TEST_CASE("credOnExec: setuid/setgid bits raise euid/egid; suid/sgid track") {
    Cred c; credInitRoot(c); c.ruid=c.euid=c.suid=c.fsuid=1000;
    c.rgid=c.egid=c.sgid=c.fsgid=1000;
    credOnExec(c, /*fileUid*/0, /*fileGid*/0, /*mode*/04755);   // setuid-root binary
    CHECK(c.euid==0); CHECK(c.fsuid==0); CHECK(c.suid==0);      // gained root euid; saved tracks
    CHECK(c.ruid==1000);                                        // real uid unchanged
    Cred d; credInitRoot(d); d.ruid=d.euid=d.suid=1000; d.rgid=d.egid=d.sgid=1000; d.fsuid=1000; d.fsgid=1000;
    credOnExec(d, 0, 0, 0755);                                  // plain exec
    CHECK(d.euid==1000); CHECK(d.suid==1000);                   // unchanged; suid:=euid
}
```
  Implementation:
```cpp
void credOnExec(Cred& c, unsigned fileUid, unsigned fileGid, unsigned mode) {
    if (mode & 04000) { c.euid = c.fsuid = fileUid; }   // S_ISUID
    if (mode & 02000) { c.egid = c.fsgid = fileGid; }   // S_ISGID
    c.suid = c.euid; c.sgid = c.egid;                   // Linux: saved sets = effective on exec
}
```

- [ ] **Step 2: Run `make test`; PASS.**
- [ ] **Step 3: Wire into `execve`** (`kernel/Exec.cpp`): before loading, enforce execute permission via the VFS (`vfs->access(path, 1)` or a new `vfs->permission(path,1)` exposed publicly — add `int Vfs::checkExec(String path)` returning `permission(path,1)`). If `< 0`, fail the execve with that errno. After the image commits, `FileStat st; vfs->lstat(path, st); credOnExec(p->cred, st.uid, st.gid, st.mode);`. Do the same in `execProgram` for the initial program.
- [ ] **Step 4: Build + boot; commit:** `git commit -am "feat(exec): enforce exec permission + honor setuid/setgid bits (credOnExec)"`

### Task 13: real `/proc/<pid>/status` Uid/Gid/Groups

**Files:** Modify `kernel/Process.h` (`ProcInfo`), `kernel/Process.cpp` (snapshot), `fs/SynthFs.cpp` (`statusString` `:176-194`); Test `tests/test_synthfs*` if present, else QEMU

- [ ] **Step 1:** Add to `ProcInfo` (`kernel/Process.h:145`): `unsigned uid, gid, euid, egid; int ngroups; unsigned groups[NGROUPS_MAX];`.
- [ ] **Step 2:** In `ProcTable::snapshot`/`infoByPid` (`kernel/Process.cpp`), copy from `Process::cred`.
- [ ] **Step 3:** In `fs/SynthFs.cpp::statusString` replace the hardcoded `"\nUid:\t0\t0\t0\t0"` (`:193`) and Gid (`:194`) with the real ids: `Uid:\t<ruid>\t<euid>\t<suid>\t<fsuid>` and similarly Gid, plus a `Groups:\t<g0> <g1> …` line.
- [ ] **Step 4:** If a `tests/test_synthfs.cpp` exists, add a case asserting the rendered lines reflect a `ProcInfo` with uid 1000; else verify in QEMU (`cat /proc/self/status`).
- [ ] **Step 5: Commit:** `git commit -am "feat(proc): /proc/<pid>/status shows real Uid/Gid/Groups"`

---

## PHASE 4 — identity databases + libc-glue (clean-room)

### Task 14: clean-room `crypt($6$)` SHA-512

**Files:** Create `user/libc-glue/crypt.c`, `tests/test_crypt.cpp`; Modify `Makefile` (TEST_MODULES if host-compilable), `libc.ndl` export list

- [ ] **Step 1: Write failing tests** with published `$6$` vectors (from the crypt SHA-512 spec; these are public test vectors, not code):

```cpp
#include "doctest.h"
extern "C" char* crypt_r(const char* key, const char* salt, void* data);
// scratch struct large enough for crypt_r's working buffer:
struct crypt_data { char out[256]; char buf[1024]; };

TEST_CASE("crypt $6$ matches the reference vector") {
    crypt_data d;
    // Reference: key "Hello world!", salt "$6$saltstring" -> known published digest.
    const char* exp = "$6$saltstring$svn8UoSVapNtMuq1ukKS4tPQd8iKwSMHWjl/O817G3uBnIFNjnQJu"
                      "esI68u4OTLiBFdcbYEdFCoEOfaS35inz1";
    char* got = crypt_r("Hello world!", "$6$saltstring", &d);
    CHECK(got != nullptr);
    CHECK(std::string(got) == exp);
}
```

- [ ] **Step 2: Run the host crypt test** (add `tests/test_crypt.cpp`; if `crypt.c` is freestanding-clean it can join `TEST_MODULES`, else compile it directly in the test). Verify FAIL.
- [ ] **Step 3: Implement `user/libc-glue/crypt.c`** — a clean-room SHA-512 + the `$6$` key-derivation per the published algorithm (rounds default 5000; honor `rounds=` in the salt). Provide `crypt()` (static buffer) and `crypt_r()`. **Write the SHA-512 core from FIPS 180-4 and the `$6$` schedule from its public description — no copied source.**
- [ ] **Step 4: Run `make test`; the vector PASSES.**
- [ ] **Step 5:** Export `crypt`/`crypt_r` from `libc.ndl` (add to its export list the way other libc symbols are exported).
- [ ] **Step 6: Commit:** `git commit -am "feat(libc): clean-room crypt(\$6\$) SHA-512 with published-vector test"`

### Task 15: `/etc/passwd` path + `/etc/group` + `/etc/shadow` + grouplist/initgroups

**Files:** Modify `user/libc-glue/pwd_grp.c`; Create `user/libc-glue/grp_shadow.c`; Modify the SDK sysroot (`<shadow.h>` if missing)

- [ ] **Step 1:** Change the passwd path in `pwd_grp.c` from `/disks/main/nanos/config/passwd` to `/etc/passwd`; tolerate field 2 = `x` (don't treat it as a hash). Keep the root fallback.
- [ ] **Step 2:** Implement in `grp_shadow.c`: `getgrnam/getgrgid/getgrent/setgrent/endgrent` parsing `/etc/group` (`name:x:gid:m1,m2,…`), filling `struct group` (`gr_mem` array). Replace the gid=0-only stub in `pwd_grp.c`.
- [ ] **Step 3:** Implement `getspnam` + `struct spwd` parsing `/etc/shadow`. If picolibc lacks `<shadow.h>`, add a minimal one to the sysroot used by the SDK (declare `struct spwd { char* sp_namp; char* sp_pwdp; long sp_lstchg; … }` + `getspnam`).
- [ ] **Step 4:** Implement `getgrouplist(user, gid, groups, ngroups)` + `initgroups(user, gid)` (initgroups calls `setgroups` with the computed list). A user's groups = `gid` + every `/etc/group` whose member list contains `user`.
- [ ] **Step 5:** Add the credential-syscall wrappers (`setresuid/getresuid/setreuid/setfsuid` + gid, `getgroups/setgroups`) and `seteuid(e){return setresuid(-1,e,-1);}` / `setegid` as libc wrappers, if the SDK libc doesn't already provide them.
- [ ] **Step 6: Verify** by building a tiny test program (or rely on the toybox `id` in Phase 6); host-compile `grp_shadow.c` against a fixture `/etc/group` if feasible, else defer validation to the QEMU acceptance.
- [ ] **Step 7: Commit:** `git commit -am "feat(libc): /etc/{passwd,group,shadow} parsing + getgrouplist/initgroups + cred wrappers"`

---

## PHASE 5 — on-disk seed (accounts, groups, shadow, sudoers, /etc symlinks)

### Task 16: seed files + image wiring + `/etc` symlink persistence

**Files:** Create `config/group`, `config/shadow`, `config/sudoers`; Modify `config/passwd`, `scripts/create-grub2-image.sh`, Makefile `_image`, the boot `/etc` populator (find it: grep `config/etc` in `kernel/`/`init`)

- [ ] **Step 1:** Generate password hashes with the new `crypt($6$)` (or `mkpasswd` from toybox once built) for `root` and `jan`. Write `config/passwd`:
```
root:x:0:0:root:/disks/main/root:/disks/main/nanos/bin/bash.nxe
jan:x:1000:1000:Jan:/disks/main/home/jan:/disks/main/nanos/bin/bash.nxe
```
  `config/shadow` (mode 0600):
```
root:$6$<roothash>:19000:0:99999:7:::
jan:$6$<janhash>:19000:0:99999:7:::
```
  `config/group`:
```
root:x:0:
daemon:x:1:
tty:x:5:
disk:x:6:
wheel:x:10:jan
jan:x:1000:
```
  `config/sudoers` (mode 0440):
```
root ALL=(ALL) ALL
%wheel ALL=(ALL) ALL
```
- [ ] **Step 2:** In `scripts/create-grub2-image.sh` / Makefile `_image`: copy the four files to `/disks/main/nanos/config/`; create `jan`'s home `/disks/main/home/jan` owned `1000:1000`; set `shadow` 0600 root, `sudoers` 0440 root; create the `/etc` symlinks **in the on-disk `config/etc/` template** so the boot populator reproduces them: `passwd→/disks/main/nanos/config/passwd`, `shadow→…/shadow`, `group→…/group`, `sudoers→…/sudoers`.
- [ ] **Step 3:** Teach the boot `/etc` populator to **preserve symlink** template entries (create them as symlinks, not copy target contents). Grep for where `config/etc` is read into the `/etc` RamFs and add symlink handling.
- [ ] **Step 4: Build the image + boot**; verify `ls -l /etc/passwd` shows a symlink and `cat /etc/group` works.
- [ ] **Step 5: e2fsck** the image after the write-bearing boot: `e2fsck -fn disk/image64-grub2.img` → clean.
- [ ] **Step 6: Commit:** `git commit -am "feat(image): seed root+jan accounts, /etc/{passwd,shadow,group,sudoers} via persistent symlinks"`

---

## PHASE 6 — toybox tools + init→login

### Task 17: port toybox (login/su/passwd/useradd/groupadd/id/groups)

**Files:** nanos-sdk port dir for toybox; Makefile toybox target; install into `/disks/main/nanos/bin`

- [ ] **Step 1:** Add a `toybox` port following the existing nanos-sdk port flow (mirror an existing port target in the Makefile, e.g. how `vim`/`grep` are built). Toybox builds from a single `.config`; enable `CONFIG_LOGIN`, `CONFIG_SU`, `CONFIG_PASSWD`, `CONFIG_USERADD`, `CONFIG_GROUPADD`, `CONFIG_ID`, `CONFIG_GROUPS`, `CONFIG_MKPASSWD`.
- [ ] **Step 2:** Build against the i686-nanos SDK; resolve missing libc symbols by pointing at the Phase-4 glue (`crypt`, `getspnam`, `getgrouplist`, `initgroups`, `setgroups`, …).
- [ ] **Step 3:** Install the multicall binary + symlinks into `/disks/main/nanos/bin`. Mark `su` and `passwd` **mode 4755 root** in the image build (so an unprivileged user can authenticate / change passwords).
- [ ] **Step 4: Boot + smoke test in QEMU** (still auto-root at this point): `id` prints `uid=0(root)`; `groups` lists root; `useradd -D` runs.
- [ ] **Step 5: Commit:** `git commit -am "feat(tools): port toybox login/su/passwd/useradd/groupadd/id/groups"`

### Task 18: `init` → `login` on the console

**Files:** Modify `user/init.c` (`:169-252`)

- [ ] **Step 1:** Replace the direct shell exec with execing `login` on the console tty (keep the re-spawn loop). `login` (toybox) handles the `login:`/`Password:` prompts, shadow verification, `setgid`/`initgroups`/`setuid`, `chdir(home)`, env (`HOME/SHELL/USER/LOGNAME/PATH`), and exec of `pw_shell`.
- [ ] **Step 2:** Ensure `init` itself stays root (it must, to spawn `login`). Confirm the controlling-tty / pgrp setup still holds (job control) — `login` execs the shell which becomes the session leader as today.
- [ ] **Step 3: Boot QEMU**: a `login:` prompt appears; log in as `jan`/<password> → shell runs as uid 1000; `id` confirms `uid=1000(jan) … groups=1000(jan),10(wheel)`.
- [ ] **Step 4:** PTY/nterm: confirm a new terminal spawns the user's shell inheriting the session creds (no re-login) — matches the spec; if nterm currently execs a shell directly, leave it (it inherits the logged-in user's `Cred` via fork).
- [ ] **Step 5: e2fsck clean; commit:** `git commit -am "feat(init): mandatory console login (init execs toybox login)"`

---

## PHASE 7 — sudo (ISC) + sudoers

### Task 19: port sudo

**Files:** nanos-sdk port dir for sudo; Makefile target; `/etc/sudoers`

- [ ] **Step 1:** Add a `sudo` port (Sudo Project, ISC) via the SDK flow. Configure a minimal build: file-based `/etc/sudoers`, no PAM (`--disable-pam`), no LDAP, the built-in `sudoers` policy plugin, `crypt`-based password auth against `/etc/shadow`.
- [ ] **Step 2:** Resolve libc-glue gaps surfaced by sudo (it uses `getpwnam`, `getgrnam`, `getgrouplist`, `setresuid`, `setgroups`, `crypt`, tty handling) — all provided by Phases 1–4. Stub/no-op the few host-management bits sudo expects that NanOS lacks (e.g. `/dev/log` syslog → write to console or drop).
- [ ] **Step 3:** Install `sudo` **mode 4755 root** into `/disks/main/nanos/bin`; install `/etc/sudoers` (0440) via the seed.
- [ ] **Step 4: QEMU acceptance:** as `jan`, `sudo cat /etc/shadow` → prompts jan's password → succeeds (jan ∈ wheel); `cat /etc/shadow` directly → `Permission denied`. As a non-wheel user, `sudo` is refused.
- [ ] **Step 5: e2fsck clean; commit:** `git commit -am "feat(tools): port sudo (ISC) with /etc/sudoers"`

---

## PHASE 8 — integration + acceptance + e2fsck

### Task 20: end-to-end QEMU acceptance

**Files:** none (verification); record the procedure in the spec's verification section if anything deviates

- [ ] **Step 1:** Boot `make run64` headless (screendump pattern from CLAUDE.md). Sequence and expected results:
  - `login:` prompt → log in as `jan` → shell as uid 1000.
  - `id` → `uid=1000(jan) gid=1000(jan) groups=1000(jan),10(wheel)`.
  - `cat /etc/shadow` → `Permission denied` (`-EACCES`).
  - `sudo cat /etc/shadow` → prompts jan's password → prints shadow.
  - `su -` → root password → `id` shows `uid=0(root)`; `exit` returns to jan.
  - In a sticky dir (`/tmp`), as jan create a file, then as root (via su) try to remove jan's file is allowed (root); as jan try to remove a root-owned file → `Permission denied` (`-EPERM`).
  - `touch /nanos/bin/x` as jan → `Permission denied`.
  - `passwd` as jan changes jan's own password; relogin works with the new password (persists across reboot).
- [ ] **Step 2:** Negative checks: jan cannot `kill` a root process (signal-permission is a separate model — note if NanOS enforces it; if not, record as out-of-scope follow-up, since this plan is *file/identity* permissions).
- [ ] **Step 3:** `e2fsck -fn disk/image64-grub2.img` on the host after the session → **clean**.
- [ ] **Step 4:** Boot log `make run64` with `-d int -D /tmp/log` → grep no `v=08/0d/0e`.
- [ ] **Step 5:** Run the full `make test` host suite → green, coverage ≥90% (the new `Cred`/VFS-perm/crypt modules are well-covered).
- [ ] **Step 6: Commit** any final fixups: `git commit -am "test: end-to-end user-permissions acceptance (login/su/sudo/sticky/e2fsck)"`

---

## Self-review notes (coverage of the spec)

- Spec Component 1 (`Cred` + pure fns) → Tasks 1–5, 12 (credOnExec). ✓
- Component 2 (credential syscalls) → Tasks 6–7. ✓
- Component 3 (VFS enforcement, path-walk, sticky, ownership, umask) → Tasks 8–10. ✓
- Component 4 (setuid/setgid at exec, fork cred, /proc) → Tasks 11–13. ✓
- Component 5 (crypt, /etc/{passwd,group,shadow}, grouplist/initgroups, /etc symlinks) → Tasks 14–16. ✓
- Component 6 (toybox + init→login) → Tasks 17–18. ✓
- Component 7 (sudo) → Task 19. ✓
- Testing (host test_cred/test_vfs_perm/test_crypt, e2fsck, QEMU acceptance) → Tasks 1–9, 14, 20. ✓
- Capabilities → explicitly out of scope (spec); no task, by design. ✓

**Type/name consistency:** `Cred` field names (`ruid/euid/suid/fsuid`, `rgid/egid/sgid/fsgid`, `ngroups`, `groups`) and the function names (`credAccess`, `credMaySticky`, `credSetresuid`, `credOnExec`, `credSetgroups`, …) are used identically across Tasks 1–13. `Vfs` helpers (`maySearch/permission/mayCreate/mayDelete`, `setCredProvider`, `statNoCheck`, `chownNoCheck`, `parentOf`) are consistent across Tasks 8–10. Syscall method names match the dispatch cases (Task 7). `NGROUPS_MAX = 32` used everywhere.

**Known iterative risks (flagged, not placeholders):** toybox/sudo ports (Tasks 17, 19) are real third-party builds — expect a few libc-glue gaps to surface and be filled from the Phase-4 unit; the `/etc` symlink-preserving populator (Task 16 Step 3) and `crypt($6$)` vector (Task 14) are the correctness-critical seams and are each gated by an explicit check.

## Verification log (checked against the live tree, 2026-06-22)

- **x86_64 syscall numbers (Task 7):** the x86_64 block (`kernel/SyscallNr.h:11-152`) uses real Linux x86_64 numbers; `113-123` (setreuid/setregid/getgroups/setgroups/setres*uid/getres*uid/setfs*uid + gids) are **unused there** → the plan's values are correct and collision-free. The `118/119/120` seen in the file are i386-block (fsync/sigreturn/clone), not x86_64. The names `setreuid/setresuid/setfsuid/get|setgroups` are **not yet defined** in either block → no redefinition.
- **i386 syscall numbers (Task 7):** i686 is FROZEN; the i386 block reassigns numbers, so the plan now instructs: real i386 number if free in that block, else a `≥1001` NanOS-private sentinel (existing pattern). Dispatch-compile only.
- **RamFs ops (Tasks 8-9):** `fs/RamFs.h:62-73` implements `create/unlink/mkdir/rmdir/rename/symlink/truncate/chmod/chown/lchown` → the RamFs-based VFS-enforcement tests are valid as written.
- **Recursion guard (Task 8):** `maySearch`/`permission` must call the private `statNoCheck` (not the gated public `stat`) — fixed in the code samples.
- **Existing creds (Tasks 6-7):** confirmed `Syscalls::m_uid/m_euid/m_gid/m_egid` at `kernel/Syscall.h:126-127`, ctor init `kernel/Syscall.cpp:51`, fork copy `:81-82`, `permCheck` `:776-788` used only by `open()` `:135-140`, `getuid..getegid` `:757-760`, `setuid/setgid` `:763-772` — all migration points exist as the plan states.
- **execve / proc hooks:** `kernel/Exec.cpp:124` (execve) and `:70` (execProgram) use `ProcTable::current()`; `fs/SynthFs.cpp:176` `statusString` hardcodes `Uid:\t0` at `:193-194` (Task 13 target). Confirmed.
- **crypt($6$) vector (Task 14):** uses the canonical published Drepper test vector (`"Hello world!"` / `$6$saltstring`) — a spec value, not copied code.

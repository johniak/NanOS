# Linux-Style User Permissions — Design Spec

**Date:** 2026-06-22
**Status:** design (awaiting user review → implementation plan)
**Topic:** full multi-user permission model for NanOS, faithful to Linux/POSIX semantics

---

## Goal

Give NanOS a complete, Linux-faithful discretionary access control (DAC) model that is
**actually exercised at runtime**: real non-root accounts, mandatory console login, `su`,
`sudo`, setuid/setgid/sticky semantics, the full saved-set-id credential family,
supplementary groups, and `/etc/shadow` password authentication. Today everything runs as
`uid 0` — this makes the existing-but-dormant credential plumbing real and enforced.

## Hard constraints (decided with the user)

1. **Scope:** full multi-user + `login`/`su`/`sudo`/`passwd`. The biggest variant.
2. **Tools = permissive ports, not GNU/GPL:** **toybox** (0BSD) for `login`, `su`, `passwd`,
   `useradd`, `groupadd`, `id`, `groups`; **real sudo** (Todd Miller, ISC license) with
   `/etc/sudoers`. These are legitimate ports of permissively-licensed upstreams.
3. **Our code is clean-room:** the kernel side (`Cred`, VFS enforcement) and the libc-glue
   (`crypt`, `getgr*`, `getspnam`, `getgrouplist`, …) are **original implementations** —
   semantics modeled on Linux/POSIX, **no copied source**. Algorithms implemented from
   published specs (e.g. the `$6$` SHA-512 crypt scheme), not from Linux/glibc code.
4. **Password storage:** `/etc/shadow` (modern split), `x` in `/etc/passwd` field 2.
5. **Boot:** mandatory `login:` prompt on the console (verify via shadow, then drop privilege).
6. **Seed accounts:** `root(0)` + `jan(1000)`, primary group `jan(1000)`, member of `wheel(10)`;
   standard system groups; `sudoers`: `%wheel ALL=(ALL) ALL`. Passwords set for both.
7. **No shortcuts.** The *entire* classic Linux DAC model is implemented and enforced.

### Scope note on Linux *capabilities* (CAP_*)

Linux capabilities are a **separate privilege-splitting subsystem layered on top of** the
classic DAC model — not part of "user permissions" in the file/ugo/setuid sense. They are
**explicitly out of scope** here and deferred. This is **not** a shortcut to the requested
model: when capabilities are absent, Linux itself uses exactly the privilege test we use —
`euid == 0` is root, root holds all privilege. Everything the user asked for (users, groups,
supplementary groups, rwx-for-ugo, setuid/setgid/sticky, the saved-set-id family,
fsuid/fsgid, shadow auth, login/su/sudo) is implemented in full. Capabilities would be a
future, independent spec.

---

## Current state (from a codebase sweep)

Already present:
- Per-process `m_uid/m_euid/m_gid/m_egid` + `m_umask` live **inside `Syscalls`**
  (`kernel/Syscall.h:125-127`); copied on fork (`kernel/Syscall.cpp:80-82`); everyone starts
  root (`kernel/Syscall.cpp:50-51`).
- `getuid/geteuid/getgid/getegid`, `setuid/setgid` (simplistic), `umask` — implemented
  (`kernel/Syscall.cpp:757-772, 659-663`).
- A single `permCheck(stat, want)` for r/w/x, called only by `open()`
  (`kernel/Syscall.cpp:776-788, 137`).
- ext inode stores 32-bit uid/gid + mode; `stat`/`fstat` return them
  (`fs/ExtFilesystem.h:82-104, 1242-1256`; `kernel/Syscall.cpp:412-422`).
- `chmod/fchmod/chown/fchown/lchown` reach the filesystem; **no caller-permission checks**.
- `passwd` parsed from `/disks/main/nanos/config/passwd`, hash inline in field 2
  (`user/libc-glue/pwd_grp.c`); `getgrgid` is a hardcoded `root`-only stub; no `/etc/group`,
  no shadow, no supplementary groups.
- `init` (PID 1, root) execs the shell directly — no login, everything stays `uid 0`
  (`user/init.c:169-252`).

Missing / to build: `setreuid/setresuid/seteuid/setfsuid` (+ gid), `getgroups/setgroups`,
`getresuid/getresgid`; saved-set-id semantics; **enforcement on every path op** (not just
`open`); directory search/write checks; **sticky bit**; **setuid/setgid honoring at exec**;
ownership rules for chmod/chown/utimes; `access()` using real ids; `/etc/group` + `/etc/shadow`
+ `getgr*`/`getspnam`/`getgrouplist`/`initgroups`; `crypt()`; login/su/sudo/passwd; real
non-root runtime; two umask bugs (`mkdir()` and `open(O_CREAT)` ignore the mask).

---

## Architecture (layered, Linux-faithful)

```
userland tools (ports)      toybox login/su/passwd/useradd/groupadd/id/groups; sudo (ISC)
        │                   read /etc/{passwd,shadow,group,sudoers}; verify via crypt();
        │                   drop/raise privilege via the credential syscalls
        ▼
libc-glue (clean-room)      getpw*/getgr*/getspnam/getpwent/getgrent, getgrouplist,
        │                   initgroups, crypt($6$), set*id/get*id/get|setgroups wrappers
        ▼  syscalls
kernel Syscalls (glue)      credential syscalls operate on Process::cred; exec honors
        │                   setuid/setgid bits; open/create/unlink/... call into the VFS
        ▼
VFS policy layer  ◄── inode_permission() analogue.  Vfs::permission()/mayCreate()/
        │             mayDelete()/maySearch()/mayChmod()/mayChown() enforce DAC using the
        │             CALLER's Cred (obtained via a CredProvider hook). Full path-walk:
        │             search (x) on ancestors → object perm → parent-write + sticky for
        │             create/delete/rename. Pure decisions delegate to kernel/Cred.cpp.
        ▼
FS drivers (mechanism)      ExtFilesystem / RamFs / SynthFs — UNCHANGED, still MI, still
                            host-tested without creds. They are the "i_op": provide
                            metadata + perform the operation. Policy is NOT here.

kernel/Cred.{h,cpp}  (MI, host-tested)   struct Cred + pure permission/transition functions.
kernel/Process.h     Process::cred is the canonical home (Linux task_struct->cred analogue).
```

**Why policy in the VFS, not in the FS drivers:** this mirrors Linux exactly (the VFS runs
`inode_permission()`; filesystems just expose inode metadata + ops). It keeps all three FS
drivers as pure mechanism (MI, host-testable without credentials, `make check-arch` clean),
concentrates the security-sensitive logic in one auditable place, and avoids threading
`const Cred&` through dozens of driver virtual signatures.

**How the VFS learns the caller:** a `CredProvider` function pointer — `const Cred* (*)()` —
set once at boot to return `ProcTable::current()->cred`. Host tests set it to a test `Cred`.
The VFS stays MI (no `ProcTable` dependency). This is the single seam between policy and the
running process.

---

## Component 1 — `struct Cred` + pure decision functions (`kernel/Cred.{h,cpp}`, MI)

```c
namespace kernel {
static const int NGROUPS = 32;
struct Cred {
    unsigned ruid, euid, suid, fsuid;     // real / effective / saved-set / filesystem
    unsigned rgid, egid, sgid, fsgid;
    int      ngroups;                     // # valid entries in groups[]
    unsigned groups[NGROUPS];             // supplementary groups
};
}
```

`Process` (`kernel/Process.h`) gains `Cred cred;` — the canonical store. `Syscalls` drops
`m_uid/m_euid/m_gid/m_egid` and instead holds `Cred* cred` pointing at its process's cred (in
host tests the test owns a `Cred` and injects the pointer, so credential logic stays testable
without the kernel). `fork` copies the parent's `Cred` into the child's `Process::cred`;
`execve` preserves it except for setuid/setgid bits (Component 4).

### Pure functions (the heart — host-tested exhaustively, no kernel deps)

- `int credAccess(const Cred&, unsigned fileUid, unsigned fileGid, unsigned mode, int want, bool useReal)`
  → `0` or `-EACCES`. Selects owner/group/other by **fsuid/fsgid** (or **ruid/rgid** when
  `useReal`, for `access(2)`); membership check scans `egid`/`fsgid` **and** supplementary
  `groups[]`. Root (`euid==0`): r/w always granted; `x` granted only if **any** execute bit is
  set (faithful to Linux root). `want` = R(4)/W(2)/X(1) bits.
- `bool credInGroup(const Cred&, unsigned gid)` — egid/fsgid + supplementary scan.
- `int credMaySticky(const Cred&, unsigned dirUid, unsigned fileUid)` — sticky-dir delete/rename
  rule: allowed iff `euid==0`, `fsuid==fileUid` (file owner), or `fsuid==dirUid` (dir owner);
  else `-EPERM`.
- `int credMayChmod(const Cred&, unsigned fileUid)` — owner (`fsuid==fileUid`) or root, else
  `-EPERM`. (Caller also clears S_ISGID when the setter is non-root and not in the file's group.)
- `int credMayChown(const Cred&, unsigned fileUid, int newUid, int newGid)` — changing owner
  (`newUid` != -1 and != current) requires root; changing group requires root **or** (owner of
  the file **and** member of `newGid`); else `-EPERM`. Non-root chown clears setuid/setgid bits.
- `int credMayUtimes(const Cred&, unsigned fileUid, bool toNow)` — `toNow` (utimes(NULL)) needs
  write perm; explicit times need owner or root.
- Transition functions, each returning a new `Cred` or `-EPERM`, encoding POSIX rules
  (privileged = `euid==0` may set anything; unprivileged may set each target only to one of its
  current real/effective/saved value); `fsuid/fsgid` follow `euid/egid` except via setfsuid/gid:
  `credSetuid, credSeteuid, credSetreuid, credSetresuid` (+ `…gid` mirrors), `credSetfsuid`,
  `credSetfsgid`, `credSetgroups` (root-only).

These are total, side-effect-free functions over plain structs → every branch is unit-tested
without QEMU or a live process.

---

## Component 2 — credential syscalls (`kernel/Syscall.{h,cpp}`, `SyscallNr.h`, dispatch)

All operate on `*cred` and delegate decisions to the Component-1 functions.

Keep: `getuid/geteuid/getgid/getegid`. Rewrite `setuid/setgid` to saved-set-id semantics.
**Add:** `getresuid/getresgid` (3 out-params), `seteuid/setegid`, `setreuid/setregid`,
`setresuid/setresgid`, `setfsuid/setfsgid`, `getgroups/setgroups`.

Wiring: add the missing numbers to `kernel/SyscallNr.h` for **both** x86_64 and i386 ABIs
(canonical Linux numbers), route them in `kernel/SyscallDispatch.cpp`, add libc wrappers
(Component 5). `getgroups(0, …)` returns the count; `setgroups` is root-only.

---

## Component 3 — DAC enforcement in the VFS (`fs/Vfs.{h,cpp}`)

`Vfs` gains a private policy layer used by every public method. The FS-driver virtuals are
untouched. A `CredProvider` hook supplies the caller's `Cred`; if unset (early boot before
PID 1), checks are bypassed (kernel context = root-equivalent), matching today's behavior.

**Full path-walk (namei):** `Vfs::maySearch(path, cred)` stats each **ancestor** directory of
`path` and requires search (`x`); root bypasses search. Run before every operation so a missing
`x` on any intermediate directory yields `-EACCES`, exactly like Linux.

Per-operation checks (object stat via the FS, then a Component-1 decision):

| Operation | Checks |
|---|---|
| `open` RDONLY | search ancestors; **R** on file |
| `open` WRONLY/RDWR | search ancestors; **W** on file (+ **W** for `O_TRUNC`) |
| `open` `O_CREAT` (new) | search + **W** on parent dir; new file owned by `fsuid`; group = parent gid if parent has S_ISGID else `fsgid`; mode `= mode & ~umask` |
| `exec` (Component 4) | search ancestors; **X** on file |
| `mkdir`/`mknod`/`create`/`symlink` | search + **W** on parent; ownership + setgid-dir inheritance as above; mode masked by umask (fixes the current `mkdir`/`open` umask bugs) |
| `link` (new name) | search + **W** on new parent; source must exist and not be a dir |
| `unlink`/`rmdir` | search + **W** on parent; **sticky**: `credMaySticky` if parent has S_ISVTX |
| `rename` | delete-checks on old parent + create-checks on new parent; sticky on both |
| `chmod`/`fchmod` | `credMayChmod`; clear S_ISGID if non-root setter not in file's group |
| `chown`/`lchown`/`fchown` | `credMayChown`; clear setuid/setgid on success when non-root |
| `truncate`/`ftruncate` | **W** on file |
| `utimes`/`utime`/`utimensat` | `credMayUtimes` |
| `stat`/`lstat`/`readlink`/`readdir`/`statfs` | search ancestors only (no perm on the object) |
| `access`/`faccessat` | `credAccess(..., useReal=true)` — uses **real** uid/gid |

Errno faithfulness: `-EACCES` for read/write/search/exec denial; `-EPERM` for ownership-
required ops (chmod/chown by non-owner), sticky violations, and privileged-only transitions.

---

## Component 4 — setuid/setgid at `execve` (`kernel/Exec.cpp`) — the linchpin

On every successful `execve` of `path`:
1. Enforce **X** on the file + search on ancestors (via the VFS).
2. Read the file's `uid`, `gid`, `mode`.
3. Always: `suid = euid`, `sgid = egid` (Linux resets the saved sets to the new effective ids
   at exec).
4. If `S_ISUID`: `euid = fsuid = file.uid`, then `suid = euid`.
5. If `S_ISGID`: `egid = fsgid = file.gid`, then `sgid = egid`.
6. `ruid/rgid` and supplementary groups are unchanged. Apply to the new image's `Process::cred`.

This is what powers privilege transitions: `su`, `sudo`, `passwd` are installed **mode 4755
root** (setuid-root) so an unprivileged caller gains root to read `/etc/shadow` and switch
identity; `login` already runs as root (PID 1's child) and drops privilege after auth.

We honor the on-disk setuid bits as-is; bits are cleared on chmod/chown by the rules in
Component 3 (so a modified setuid binary loses its bit, as on Linux). glibc-style `AT_SECURE`
env sanitization is **not** relied upon for security (sudo/su/login re-validate independently);
noted, not implemented.

`/proc/<pid>/status` gains faithful `Uid:`, `Gid:`, `Groups:` lines: `ProcInfo`
(`kernel/Process.h`) carries a cred snapshot; `SynthFs` renders them. Feeds `id`/`ps`.

---

## Component 5 — identity databases + libc-glue (clean-room)

### On-disk account database (persistent, on the ext disk)

Canonical files under `/disks/main/nanos/config/`:
- `passwd` — `name:x:uid:gid:gecos:home:shell` (hash → shadow, field 2 = `x`).
- `shadow` — `name:$6$salt$hash:lastchg:min:max:warn:inact:expire:`; mode **0600 root:root**.
- `group` — `name:x:gid:member,member,…`.
- `sudoers` — `root ALL=(ALL) ALL` + `%wheel ALL=(ALL) ALL`; mode **0440 root**.

### `/etc` visibility (persistence vs the tmpfs `/etc`)

`/etc` is a RamFs tmpfs populated at boot from the `config/etc/` template (see
`docs/en/filesystem.md`), so it is cleared each reboot. Tools (toybox, sudo) hardcode
`/etc/passwd` etc., and account edits **must persist**. Solution: `/etc/{passwd,shadow,group,
sudoers}` are **symlinks into `config/`** (the ext driver already resolves symlinks). Reads via
`/etc/*` reach the persistent on-disk files; `passwd`/`useradd` writes land on disk and survive
reboot. The boot `/etc` populator is taught to **preserve symlink** template entries (rather
than copying their targets' contents). Only these four files are persistent-via-link; the rest
of `/etc` stays transient tmpfs.

### libc-glue (`user/libc-glue/pwd_grp.c` + a new `shadow`/`crypt` unit)

Clean-room, modeled on POSIX:
- `getpwnam/getpwuid/getpwent/setpwent/endpwent` — read `/etc/passwd`, tolerate `x` in field 2.
- `getgrnam/getgrgid/getgrent/setgrent/endgrent` — parse `/etc/group` **with member lists**
  (replaces the gid=0-only stub).
- `getspnam` (+ `struct spwd`) — parse `/etc/shadow`. Add `<shadow.h>` to the SDK sysroot if
  picolibc lacks it.
- `getgrouplist`/`initgroups` — a user's groups = primary gid + every `/etc/group` listing them.
- `crypt`/`crypt_r` — **clean-room SHA-512 `$6$`** implemented from the published crypt scheme
  (recognize `$6$`; optionally `$5$`/`$1$`). Export through `libc.ndl`.
- Thin wrappers for the new syscalls: `setresuid/getresuid/seteuid/setreuid/setfsuid` (+gid),
  `getgroups/setgroups`.

---

## Component 6 — session tools + init flow (ports)

- **toybox** (0BSD) via the existing nanos-sdk port flow: enable `login`, `su`, `passwd`,
  `useradd`, `groupadd`, `id`, `groups` (+ `mkpasswd`). Installed in `/disks/main/nanos/bin`.
  `su` and `passwd` installed **mode 4755 root** (setuid-root). `login` runs as root.
- **sudo** (ISC) ported with `/etc/sudoers`, installed **mode 4755 root**. Verifies the
  invoking user's password (`getspnam` + `crypt`), authorizes via `%wheel`.
- **`init` (`user/init.c`)**: PID 1 (root) no longer execs the shell. It opens the console tty
  and execs **`login`** there. `login` prompts `login:` + `Password:`, verifies via shadow, then
  `setgid(pw_gid)` → `initgroups(name, pw_gid)` → `setuid(pw_uid)` → `chdir(home)` → set
  `HOME/SHELL/USER/LOGNAME/PATH` → exec the login shell from `pw_shell`. On shell exit, `init`
  re-spawns `login` (loop). **PTY/nterm:** the console requires login; an nterm/PTY terminal
  spawns the user's shell inheriting the already-authenticated session creds (faithful: a
  graphical terminal does not re-authenticate). 
- **Seed (image build, `scripts/create-image.sh` + Makefile `_image`):** create
  `config/{passwd,shadow,group,sudoers}` and the `/etc` symlinks; accounts `root(0)` and
  `jan(1000)` (group `jan(1000)`, member of `wheel(10)`); standard system groups
  (`daemon, tty, disk, …`); passwords set for both (hashed with our `$6$` crypt).

---

## Component 7 — testing & verification (no shortcuts)

Host tests (doctest, `nanos-test` container, ≥90% line-coverage gate; add modules to
`TEST_MODULES`/`COV_PATTERNS`; `make check-arch` stays clean — `Cred` is MI):
- `tests/test_cred.cpp` — exhaustive over `credAccess` (every ugo × want × root, real-vs-fs),
  `credInGroup`, `credMaySticky`, `credMayChmod`, `credMayChown`, `credMayUtimes`, and **every
  branch** of the set*id transition functions (privileged + unprivileged) + `setgroups`.
- `tests/test_vfs_perm.cpp` — VFS enforcement against ext/RamFs fixtures with a stubbed
  `CredProvider`: read/write/exec allow+deny, path-walk search denied on a no-`x` ancestor,
  create/unlink/rename parent-write + sticky, chmod/chown ownership (`-EPERM` vs `-EACCES`),
  `access()` uses real ids, umask applied on create/mkdir, setuid/setgid-bit exec transitions.
- `tests/test_crypt.cpp` — published `$6$` test vectors must match our `crypt()`.
- ext write path stays **e2fsck-clean** after passwd/useradd/shadow writes (dump + `e2fsck -fn`).

QEMU headless acceptance (the real proof): boot → `login:` → log in as `jan` (password) →
`id` shows `uid=1000(jan) gid=1000(jan) groups=1000(jan),10(wheel)` → `cat /etc/shadow` →
**Permission denied** → `sudo cat /etc/shadow` → prompts jan's password → succeeds →
`su -` → root prompt → create a root-owned file → as `jan`, denied to remove it from a sticky
dir, denied to write `/nanos/bin`, denied to `kill` a root process. Screenshot-verified. Boot
log (`-d int`) free of `v=08/0d/0e` faults.

---

## Implementation phasing (for the plan; each phase: host-tests + e2fsck + build + QEMU + commit)

1. **`Cred` core + credential syscalls** — host-tested; no behavior change yet (all still root).
2. **VFS enforcement layer + `CredProvider` + umask fixes** — DAC enforced; root-only runtime
   still works because PID 1 is root.
3. **setuid/setgid honoring at exec + `access()` real-id + faithful errno + `/proc` cred lines.**
4. **libc-glue:** `/etc/group` + `/etc/shadow`, `getgr*`/`getspnam`/`getgrouplist`/`initgroups`,
   `crypt($6$)`.
5. **On-disk seed:** `passwd`/`shadow`/`group`/`sudoers` + `/etc` symlinks + `root`/`jan` accounts.
6. **toybox tools** (`login/su/passwd/useradd/groupadd/id/groups`) + `init → login` flow.
7. **sudo** (ISC) + `/etc/sudoers`.
8. **Integration + QEMU acceptance + e2fsck-clean.**

## Files (new / changed)

- **New:** `kernel/Cred.{h,cpp}`; `tests/test_cred.cpp`, `tests/test_vfs_perm.cpp`,
  `tests/test_crypt.cpp`; libc-glue `crypt`/`shadow` unit; toybox + sudo port dirs (nanos-sdk).
- **Changed:** `kernel/Process.h` (`Cred cred`, `ProcInfo` cred snapshot), `kernel/Syscall.{h,cpp}`
  (drop `m_*` ids → `Cred* cred`; new credential syscalls), `kernel/SyscallNr.h`,
  `kernel/SyscallDispatch.cpp`, `kernel/Exec.cpp` (setuid-bit exec), `fs/Vfs.{h,cpp}` (policy
  layer + `CredProvider`), `kernel/Kernel.cpp` (install `CredProvider`), `user/init.c`
  (`→ login`), `user/libc-glue/pwd_grp.c` (+group/shadow/grouplist), `scripts/create-image.sh`
  + Makefile `_image` (seed + `/etc` symlinks), `/etc` boot populator (preserve symlinks),
  `Makefile` (`TEST_MODULES`/`COV_PATTERNS`, toybox/sudo port targets).

## Invariants / risks

- FS drivers stay **MI** and **mechanism-only** (`make check-arch` clean); all policy in the VFS.
- `Cred` + decisions are **pure** → fully host-tested before any QEMU run.
- Boot ordering: enforcement must no-op until `CredProvider` is installed (kernel-context ops
  before PID 1 must not be denied).
- Biggest risks: the `/etc` symlink-persistence seam (Component 5) and `crypt($6$)` correctness
  (Component 7 test vectors) and the setuid-exec transition (Component 4) — each gated by tests.
- Mounting the system disk read-write already exists; passwd/useradd writes go to the real disk
  → `e2fsck -fn` after every write-bearing test.

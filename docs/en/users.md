# NanOS User Permissions

NanOS implements the classic Linux/POSIX **discretionary access control (DAC)** model and
runs it for real: there are genuine non-root accounts, a mandatory console login, `su`,
`sudo`, setuid/setgid/sticky semantics, the full saved-set-id credential family,
supplementary groups, and `/etc/shadow` password authentication. This document describes
how that model is wired through the kernel, the VFS, the syscall layer, the libc-glue, the
on-disk account database, and the ported session tools.

Two ideas underpin everything and are worth separating up front:

1. **The credential record (`struct Cred`)** — the per-process identity (real / effective /
   saved-set / filesystem uid+gid + supplementary groups), the analogue of Linux's
   `task_struct->cred`. It lives on `Process` and is the single source of truth for "who is
   this process".
2. **The policy layer (the VFS)** — every path operation is checked against the caller's
   `Cred` in `fs/Vfs.cpp`, exactly as Linux runs `inode_permission()` in the VFS. The
   filesystem drivers stay pure mechanism: they expose inode metadata and perform the
   operation, but never decide *whether* it is allowed.

This mirrors Linux: the security-sensitive decisions are concentrated in one auditable
place, the FS drivers stay machine-independent and host-testable without credentials
(`make check-arch` clean), and the pure decision logic is unit-tested exhaustively.

> Scope note: Linux *capabilities* (`CAP_*`) are a separate privilege-splitting subsystem
> layered on top of classic DAC, and are out of scope here. With capabilities absent, NanOS
> uses exactly the test Linux itself uses in that case — `euid == 0` is root, root holds all
> privilege. Everything in the ugo/setuid/saved-set-id/shadow/login/su/sudo model is
> implemented in full.

---

## 1. `struct Cred` — the credential record (`kernel/Cred.h`)

```c
namespace kernel {
static const int NGROUPS_MAX = 32;
struct Cred {
    unsigned ruid, euid, suid, fsuid;   // real / effective / saved-set / filesystem uid
    unsigned rgid, egid, sgid, fsgid;   // ... gids
    int      ngroups;                   // valid entries in groups[]
    unsigned groups[NGROUPS_MAX];       // supplementary groups
};
}
```

`Process` (`kernel/Process.h:92`) owns a `Cred cred;` — the canonical store, the Linux
`task_struct->cred` analogue, initialised to root by `ProcTable::alloc`. The per-process
`Syscalls` object holds a `Cred*` that points at its process's `cred` (in host tests the
test owns a `Cred` and injects the pointer, so the credential logic is testable without a
live kernel); `Syscalls::setCred()` re-points it.

- **fork** copies the parent's `Cred` into the child's `Process::cred` and re-points the
  child's `Syscalls` at it (`kernel/Exec.cpp:254`).
- **execve** preserves it except for the file's setuid/setgid bits (§4).

`Cred` is plain data with no methods — all policy lives in the pure functions below, which
is what makes it machine-independent and host-tested.

---

## 2. The pure decision functions (`kernel/Cred.cpp`)

Every access/ownership/transition rule is a total, side-effect-free function over plain
structs, so every branch is unit-tested without QEMU (`tests/test_cred.cpp`). errno values
are guarded so the file compiles standalone for the host test.

### Access

`int credAccess(const Cred&, fileUid, fileGid, mode, want, useReal)` → `0` or `-EACCES`
(`kernel/Cred.cpp:38`). `want` is the bitmask `R(4) | W(2) | X(1)`.

- It selects the **owner / group / other** rwx triad by comparing the file's owner against
  **fsuid** (normal opens) or **ruid** (`access(2)`, when `useReal` is set), and the group
  via `egid`/`fsgid` (or `rgid` for `useReal`) **plus** the supplementary `groups[]`.
- **Root override** follows the *same* id set as the check: a setuid-root program calling
  `access(2)` is judged by its real uid, while a normal open uses fsuid. Root gets r/w
  unconditionally; **x is granted only if at least one execute bit is set anywhere** in the
  mode — faithful to Linux root.

### Ownership, sticky, metadata

| Function | Rule | Returns |
|---|---|---|
| `credInGroup(c, gid)` | `egid`/`fsgid` + supplementary scan | bool |
| `credMaySticky(c, dirUid, fileUid)` | sticky-dir delete/rename: allowed iff `euid==0`, `fsuid==fileUid`, or `fsuid==dirUid` | `0` / `-EPERM` |
| `credMayChmod(c, fileUid)` | owner (`fsuid==fileUid`) or root | `0` / `-EPERM` |
| `credMayChown(c, fileUid, newUid, newGid)` | changing owner → root only; changing group → root, or (own the file **and** be a member of `newGid`) | `0` / `-EPERM` |
| `credMayUtimes(c, fileUid, toNow)` | owner/root always; `toNow` (`utimes(NULL)`) also allowed (the VFS additionally checks write) | `0` / `-EPERM` |

### Transitions (the set\*id family)

Each mutates the `Cred` in place and returns `0`, or returns `-EPERM` and leaves it
unchanged. A `-1` argument means "leave this id unchanged" (the POSIX `setreuid`/`setresuid`
convention). The privileged path is `euid == 0` (may set anything); the unprivileged path
may set each target only to one of its current real/effective/saved values. `fsuid`/`fsgid`
follow `euid`/`egid` except via `setfsuid`/`setfsgid`.

`credSetuid`, `credSeteuid`, `credSetreuid`, `credSetresuid` (+ the `…gid` mirrors),
`credSetfsuid`/`credSetfsgid` (each returns the *previous* fsuid/fsgid, Linux semantics),
and `credSetgroups` (root-only; `-EPERM` otherwise, `-EINVAL` if `n` is out of range).

---

## 3. DAC enforcement in the VFS (`fs/Vfs.cpp`)

`Vfs` gains a private policy layer used by every public method; the FS-driver virtuals are
untouched. The caller's credentials come from a **`CredProvider`** hook —
`typedef const Cred* (*CredProviderFn)()` (`fs/Vfs.h:124`). A **null provider bypasses all
checks** (kernel context / early boot before PID 1 = root-equivalent), matching the
pre-permissions behaviour. The kernel installs the real provider at boot
(`kernel/SyscallDispatch.cpp:1096`), returning `ProcTable::current()->cred`
(`currentCred()` at `:1084`).

### The path-walk (namei)

`Vfs::maySearch(path)` (`fs/Vfs.cpp:134`) stats every **ancestor** directory of `path` and
requires search (`x`) on each. It runs before every operation, so a missing `x` on any
intermediate directory yields `-EACCES`, exactly like Linux. The per-operation check then
stats the target object and applies a Component-2 decision.

### How a permission check flows

```
syscall, e.g. open("/users/jan/notes", O_WRONLY)
   │
   ▼
Syscalls (kernel/Syscall.cpp)         resolve path vs cwd, then call the VFS
   │
   ▼
Vfs::write / read / create / ...      THE POLICY LAYER (inode_permission analogue)
   │   caller() ── CredProvider ──▶ ProcTable::current()->cred     (null ⇒ kernel/root: skip)
   │
   ├─ maySearch(path)      x on EVERY ancestor dir ─┐
   │                                                 │  each step stats the node:
   ├─ permission/mayCreate/mayDelete on the object ──┤    statNoCheck() → uid/gid/mode
   │                                                 ▼
   │                       credAccess(cred, uid, gid, mode, want, useReal)
   │                       credMaySticky / credMayChmod / credMayChown   (kernel/Cred.cpp)
   │                                                 │   pure, host-tested
   │                                                 ▼
   │                                       0   or   -EACCES / -EPERM
   ▼
FS driver (ExtFilesystem / RamFs / SynthFs)   pure MECHANISM: perform the op, no policy
```

### Per-operation checks

| Operation | Checks (`fs/Vfs.cpp`) |
|---|---|
| `read` / `readdir` | search ancestors; **R** on the file/dir |
| `write` / `truncate` | search ancestors; **W** on the file |
| `create` (new) | search + **W** on the parent; new file gets caller identity (see below) |
| `create` (existing, truncate) | **W** on the file |
| `mkdir` / `mknod` / `symlink` | search + **W** on the parent; ownership + setgid-dir inheritance |
| `unlink` / `rmdir` | search + **W** on the parent; **sticky**: `credMaySticky` if the parent has `S_ISVTX` |
| `rename` | delete-checks on the old parent + create-checks on the new parent |
| `link` | search on the source + create-checks on the new name |
| `chmod` | `credMayChmod`; a non-root setter not in the file's group has `S_ISGID` stripped |
| `chown` / `lchown` | `credMayChown`; a successful non-root chown clears the setuid/setgid bits |
| `utimes` | `credMayUtimes`, falling back to the write-permission rule for the "now" case |
| `stat` / `lstat` / `readlink` | search ancestors only (no permission on the object) |
| `access` (via `Syscalls::permCheck`) | `credAccess(..., useReal=true)` — uses **real** ids |

Errno faithfulness: `-EACCES` for read/write/search/exec denial; `-EPERM` for
ownership-required ops (chmod/chown by a non-owner), sticky violations, and privileged-only
transitions.

**New-object identity** (`Vfs::ownNewObject`, `fs/Vfs.cpp:303`): a freshly created file/dir
is stamped with the caller's `fsuid`; its group is the parent directory's group when the
parent is setgid, else the caller's `fsgid`; a setgid parent also propagates its `S_ISGID`
bit onto a new sub-directory (BSD/Linux semantics). This is also where the `mkdir`/`open`
umask handling was fixed.

---

## 4. setuid/setgid at `execve` (`kernel/Exec.cpp`) — the linchpin

`execve` enforces **X** on the file (`vfs->checkExec()`, `kernel/Exec.cpp:145`) and, after
the new image has loaded successfully (past the point of no return), applies the file's
setuid/setgid bits via `credOnExec(p->cred, st.uid, st.gid, st.mode)` (`:215`).

`credOnExec` (`kernel/Cred.cpp:75`):
1. If `S_ISUID`: `euid = fsuid = file.uid`.
2. If `S_ISGID`: `egid = fsgid = file.gid`.
3. Always: `suid = euid`, `sgid = egid` (Linux resets the saved sets to the new effective
   ids at exec). `ruid`/`rgid` and the supplementary groups are unchanged.

This powers privilege transitions: `su`, `sudo`, `passwd`, `chsh` are installed
**setuid-root**, so an unprivileged caller gains root to read `/etc/shadow` and switch
identity; `login` already runs as root (a child of PID 1) and *drops* privilege after
authentication. On-disk setuid bits are honoured as-is, and are cleared on chmod/chown by a
non-root setter (§3), so a modified setuid binary loses its bit, as on Linux.

**`/proc/<pid>/status`** carries faithful `Uid:`, `Gid:`, `Groups:` lines: `ProcInfo`
(`kernel/Process.h:165`) holds a credential snapshot and `SynthFs` renders all four ids per
line (`fs/SynthFs.cpp:193`). This feeds `id`, `ps`, and similar tools.

---

## 5. Credential syscalls + libc wrappers

The credential syscalls live on the host-testable `Syscalls` core (`kernel/Syscall.cpp:771`)
and delegate every decision to the §2 functions:

`getuid`/`geteuid`/`getgid`/`getegid` (never fail), `setuid`/`setgid`, `seteuid`/`setegid`,
`setreuid`/`setregid`, `setresuid`/`setresgid`, `getresuid`/`getresgid` (3 out-params),
`setfsuid`/`setfsgid` (return the previous fs id), `getgroups` (with `size==0` returning the
count) and `setgroups` (root-only).

They are wired into both ABIs in `kernel/SyscallNr.h` with the canonical Linux numbers — the
x86_64 set (`SYS_setreuid 113 … SYS_setfsgid 123`, plus `getgroups 115`/`setgroups 116`) and
the i386 set (`setreuid 70 … setfsgid 139 … setresuid 164 …`, plus the `*32` variants used by
16-bit-uid callers) — and routed in `kernel/SyscallDispatch.cpp:737-783`.

### The libc wrappers and the `(uid_t)-1` sentinel (`user/libc-glue/syscalls.c`)

The wrappers issue the real syscalls so userland sees and changes the kernel's per-process
`Cred`. The subtle part is the **"leave unchanged" sentinel**: POSIX spells it `(uid_t)-1`,
but picolibc's `uid_t`/`gid_t` are **16-bit**, so `(uid_t)-1 == 65535` — and the kernel
recognises only `-1`. A plain cast would hand the kernel `65535` (a real id) and *change*
the id to it. The `idarg()` helper (`user/libc-glue/syscalls.c:105`) maps the 16-bit
sentinel back to `-1`:

```c
static int idarg(uid_t v) { return v == (uid_t) -1 ? -1 : (int) v; }
```

so `setreuid`/`setresuid`/`setfsuid` (+gid) honour the sentinel. This was the real cause of
sudo's "unable to change to root gid": `setresgid((gid_t)-1, …)` was switching the id to
65535. `seteuid`/`setegid` route through `setresuid`/`setresgid` (the glibc convention).

---

## 6. The account database

### Canonical files (persistent, on the ext disk under `/disks/main/nanos/config/`)

| File | Format | Mode |
|---|---|---|
| `passwd` | `name:x:uid:gid:gecos:home:shell` (field 2 is always `x`; the hash is in shadow) | 0644 |
| `shadow` | `name:$6$salt$hash:lastchg:min:max:warn:inact:expire:flag` | 0600 root |
| `group` | `name:x:gid:member,member,…` | 0644 |
| `sudoers` | `root ALL=(ALL) ALL` + the `%wheel` rule | 0440 root |

The seed accounts are `root(0)` and `jan(1000)`, primary group `jan(1000)`, with `jan` a
member of `wheel(10)`; `jan`'s home is `/disks/main/users/jan` (NanOS keeps user homes under
**`/users`**, the macOS/Plan 9 layout, not Linux `/home`). The image build seeds the four DB
files, creates `/users/jan` (1000:1000), and seeds `~/.bashrc` from `config/skel`
(the `/etc/skel` analogue).

### `/etc` visibility (the tmpfs vs. persistence)

`/etc` is a writable **RamFs tmpfs** that is *populated at boot* from `/disks/main/nanos/
config/` by `populateEtc()` (`kernel/Kernel.cpp:158`). It copies `passwd`, `group`, `shadow`,
`sudoers` (plus the network/login templates from `config/etc/`) into the tmpfs and then
**forces the intended mode and root ownership** — `shadow` to 0600 and `sudoers` to 0440 —
so an unprivileged user cannot read the hashes or the sudo policy even though `create()`
masked the mode through umask. Because `/etc` is a tmpfs, runtime edits there are *not*
persistent: a tool that must change an account writes the on-disk copy under `/nanos/config`
(see `chsh`, §7). See [filesystem.md](filesystem.md) for the full `/etc` picture.

### `crypt($6$)` (`user/libc-glue/crypt.c`)

A self-contained, clean-room SHA-512 `$6$` implementation: a FIPS 180-4 SHA-512 core plus
the published sha512-crypt key-stretching. The output matches `openssl passwd -6` / glibc
byte-for-byte (verified against published test vectors in `tests/test_crypt.cpp`), so a hash
generated by either verifies here. Only `$6$` is implemented (the Linux default); any other
prefix returns `NULL` rather than risk a wrong match. Exported through `libc.ndl`.

### `/etc/group` and `/etc/shadow` access (`user/libc-glue/grp_shadow.c`)

Clean-room, modelled on the POSIX contracts: `getgrnam`/`getgrgid`/`getgrent` parse
`/etc/group` *with member lists*; `getspnam`/`getspent` parse `/etc/shadow` (NanOS adds a
`<shadow.h>` to the SDK sysroot, which picolibc lacks); `getgrouplist`/`initgroups` compute a
user's groups as their primary gid plus every `/etc/group` that lists them.
`user/libc-glue/pwd_grp.c` reads `/etc/passwd` (tolerating the `x` in field 2) and provides
the reentrant `getpw*_r` variants, with a built-in root fallback if the file is unreadable.

---

## 7. Shell selection (the Linux way) and `chsh`

The login shell is **per-user**, stored in `/etc/passwd` field 7 (`pw_shell`), exactly as on
Linux. `login`, `su`, `init`, and the terminal launchers all read it via `getpwnam()` /
`getpwuid()->pw_shell` and exec it. The seed has `root`'s shell as `nsh` and `jan`'s as
**bash**.

`jan`'s shell is the **bundle real path** `/disks/main/apps/bash/bash.nxe`, *not* the
`/disks/main/bin/bash.nxe` link-farm symlink — because `execve` does not follow a
final-component symlink, so PID 1 must name the real binary.

`/etc/shells` (`config/etc/shells`) is the registry of valid login shells; `chsh`
(`user/chsh.c`) restricts a user's choice to a shell listed there, and a non-root user may
change only their *own* shell. `chsh` is installed setuid-root (mode 0104755) and, on a
change, writes **both** the on-disk DB (`/disks/main/nanos/config/passwd`, so the change
survives reboot) and the live `/etc/passwd` tmpfs copy (so `getpwnam()` sees it immediately
this session). `/etc/profile` (sourced by bash login shells) sets `PATH`, an Ubuntu-style
coloured `PS1`, and re-exports `VIMRUNTIME`/`VIMINIT` (the login flow resets the
environment).

---

## 8. Session tools (ports) and the login flow

- **toybox** (0BSD multicall, x86_64 only) provides `login`, `su`, `passwd`, `id`, `groups`,
  `whoami`. It is installed as **one setuid-root binary** `toybox.nxe` (mode 0104755) with a
  per-command **hard-link farm** — `debugfs ln` creates `login.nxe`/`su.nxe`/… as hard links
  to it (`Makefile` `_image64`). toybox's `CONFIG_TOYBOX_SUID` drops privilege for the
  non-suid applets (`id`/`groups`/`whoami`) while `login`/`su`/`passwd` keep root to read
  `/etc/shadow` and switch identity.
- **sudo** (Todd Miller, ISC license, x86_64 only) is the real sudo with the sudoers policy
  linked statically (no PAM). Installed setuid-root (mode 0104755); the seeded
  `/etc/sudoers` authorises `%wheel`. NanOS-specific configuration in `config/sudoers`:
  `Defaults !use_pty` (NanOS cannot allocate sudo's command pty), `secure_path` pointing at
  the NanOS bin dirs, and `env_keep += "VIMRUNTIME VIMINIT"` (so `sudo vim` finds its runtime
  through sudo's `env_reset`). `%wheel` is currently `NOPASSWD` — sudo's interactive
  `/dev/tty` password prompt is an open item, so authorisation is by group membership only
  for now (the password path is verified separately in a GUI terminal).
- **`init` (`user/init.c`)** — PID 1 (root) no longer execs a shell directly. It opens the
  console tty and execs toybox **`login`**, which prompts `login:` + `Password:`, verifies
  against shadow with `crypt()`, then `setgid(pw_gid)` → `initgroups(name, pw_gid)` →
  `setuid(pw_uid)` → `chdir(home)` → sets `HOME`/`SHELL`/`USER`/`PATH` → execs the login
  shell from `pw_shell`. On shell exit, `init` respawns `login` (getty-style). A PTY/nterm
  terminal inherits the already-authenticated session credentials rather than
  re-authenticating, which is faithful to how a graphical terminal behaves.

---

## 9. The `.nxe` exec fallback and the app-bundle `+x` fix

Two small but load-bearing details make the tools resolve and run under DAC.

**The `.nxe` suffix fallback.** `Syscalls::nxeAppend()` (`kernel/Syscall.cpp:423`) appends
`.nxe` to a path that lacks it. `stat`, `access`, and `execve` retry `path + ".nxe"` when the
bare name is absent (`kernel/Syscall.cpp:435`, `:641`; `kernel/Exec.cpp:135`), so
`sudo vim` / a bare `vim` resolve `vim.nxe`. This **never** affects file creation — only
lookup/exec of an existing program.

**App-bundle binaries need an explicit `+x`.** Bundle binaries (`/apps/<name>/<name>.nxe`
for vim, doom, netsurf, bash, …) are written into the image with `debugfs write`, which
produces mode 0644. With VFS DAC now enforcing the execute bit, `checkExec` follows the
`/bin/<name>.nxe` symlink to the real binary and denies exec (`-EACCES`) → "vim: Permission
denied", and sudo's `access(X_OK)` over `secure_path` skips it → "sudo: vim: command not
found". The fix sets `mode 0100755` on every bundle binary
(`debugfs set_inode_field … mode 0100755`) in both image targets, alongside the existing
`+x` on the system binaries and `init.nxe`.

---

## 10. Testing & verification

Host tests (doctest, the `nanos-test` container, ≥90% line-coverage gate; `Cred` is MI so it
links into the kernel *and* the tests, and `make check-arch` stays clean):

- `tests/test_cred.cpp` — exhaustive over `credAccess` (every ugo × want × root,
  real-vs-fs), `credInGroup`, `credMaySticky`, `credMayChmod`, `credMayChown`,
  `credMayUtimes`, and every branch of the set\*id transitions (privileged + unprivileged) +
  `setgroups`.
- `tests/test_vfs_perm.cpp` — VFS enforcement against ext/RamFs fixtures with a stubbed
  `CredProvider`: read/write/exec allow+deny, path-walk search denied on a no-`x` ancestor,
  create/unlink/rename parent-write + sticky, chmod/chown ownership (`-EPERM` vs `-EACCES`),
  `access()` using real ids, umask on create/mkdir, and the setuid/setgid-bit exec
  transitions.
- `tests/test_crypt.cpp` — published `$6$` test vectors must match `crypt()`.

QEMU acceptance: boot → `login:` → log in as `jan` → `id` shows
`uid=1000(jan) gid=1000(jan) groups=1000(jan),10(wheel)` → `cat /etc/shadow` is **Permission
denied** → `sudo id` runs as root → `su -` reaches a root prompt → as `jan`, denied to remove
a root-owned file from a sticky dir and denied to write `/nanos/bin`.

---

## See also

- [filesystem.md](filesystem.md) — the VFS namespace, the on-disk layout, the `/etc` tmpfs,
  symlinks/hardlinks (the `/bin` link farm and the toybox hard-link farm).
- [syscalls.md](syscalls.md) — the syscall ABI, the `Syscalls` core, dispatch, and errno.
- [nxe-ndl.md](nxe-ndl.md) — the `.nxe` format and the loader, the ring-3 process lifecycle,
  and `libc.ndl` (which exports `crypt` and the credential wrappers).
</content>
</invoke>

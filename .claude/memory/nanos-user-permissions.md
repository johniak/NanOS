---
name: nanos-user-permissions
description: Linux-style user permissions for NanOS — Phases 1-7 DONE+QEMU-verified (kernel DAC + toybox login/su/id + init→login + WORKING sudo); password-prompt /dev/tty is the only open item
metadata: 
  node_type: memory
  type: project
  originSessionId: 661d85b4-63ba-4deb-8adc-c850a1d3125e
---

Full Linux/POSIX discretionary access control for NanOS, branch `feat/linux-user-permissions`.
Clean-room kernel+libc (no copied Linux/glibc source); tools = permissive ports (toybox 0BSD, sudo ISC).
Spec: docs/superpowers/specs/2026-06-22-linux-user-permissions-design.md; plan: docs/superpowers/plans/2026-06-22-linux-user-permissions.md.

**DONE + verified (Phases 1–5):**
- `kernel/Cred.{h,cpp}` — Cred struct (ruid/euid/suid/fsuid + gid + 32 supplementary groups) + pure decision/transition fns (credAccess, sticky/chmod/chown/utimes, credOnExec, full set*id family). Host-tested in tests/test_cred.cpp.
- Credential syscalls: setre/setres/setfs uid+gid, getres uid+gid, get/setgroups — real Linux x86_64 (113-123) + i386 numbers, dispatch wired. `Syscalls` holds `Cred*`→`Process::cred`.
- VFS DAC enforcement (fs/Vfs.cpp) = inode_permission analogue: path-walk search, read/write/create/delete/rename/chmod/chown/sticky, via a `CredProvider` hook (installSyscalls registers it). FS drivers stay pure mechanism. RamFs widened to 07777 (sticky/setuid bits). umask fixed in open()/mkdir().
- setuid/setgid honored at execve (credOnExec); fork copies Process::cred. /proc/<pid>/status shows real Uid/Gid/Groups.
- libc-glue (user/libc-glue): real getuid/setuid/... syscall wrappers (were single-user-root STUBS in posixstubs.c — that was the key gap), grp_shadow.c (/etc/group + /etc/shadow + getgrouplist/initgroups), pwd reads /etc/passwd, new include/shadow.h. crypt($6$) already existed.
- Seed: config/{passwd(x),shadow,group,sudoers}; root pw="nanos", jan(1000,wheel) pw="jan"; image installs to /nanos/config + creates /home/jan; Kernel populateEtc loads into /etc tmpfs (shadow 0600, sudoers 0440).
- **Cred.o must be in BOTH TEST_MODULES and MI_SOURCES** (missing from MI_SOURCES first → kernel link failed). Binaries must be +x in the image (debugfs writes 0644; execve enforces X even for root → set_inode_field 0100755 on init/sys-progs/bash, else boot stalls).
- 702 host tests pass (91.4%); x86_64 kernel+image build clean; QEMU boots to shell, `cat /etc/passwd`+`cat /etc/shadow` work (root-readable).

**Phase 6 DONE + QEMU-verified:** toybox 0.8.11 (0BSD) ported — login/su/passwd/id/groups/whoami as ONE setuid-root toybox.nxe + hard-link command farm (CONFIG_TOYBOX_SUID). `make ARCH=x86_64 toybox` → bin/toybox.nxe; source ~/Projects/nanos-sdk-work/toybox-0.8.11; recipe in scripts/nx-port-build.sh (patches: include sys/statfs.h; __linux__||__nanos__ in portability.c; strip .nxe from argv[0] dispatch). nx-gcc got `-xnone` before libc.ndl.a (toybox lib-probe was compiling the archive as C). libc-glue gaps filled: dprintf/getpw*_r/cfsetspeed/getgrouplist-decl/xattr-stubs/sys-headers/sigjmp_buf/_PATH_DEFPATH/SYS_renameat2. **Use HARD LINKS not symlinks for the command farm — NanOS execve does not follow a symlink on the final path component (reads the link content, not the target).** init→login wired in user/init.c. Login shells = nsh.nxe (bash not staged in the minimal image). VERIFIED: boot→'nanos login:'→jan/jan→logged in; id=uid 1000(jan) groups 1000(jan),10(wheel); pwd=/home/jan; su jan switches creds; jan denied /etc/shadow (EACCES).

**Phase 7 sudo (ISC) — PORTED, runtime bug remains:** sudo 1.9.15p5 cross-builds via nanos-sdk (`make ARCH=x86_64 sudo`; source ~/Projects/nanos-sdk-work/sudo-1.9.15p5; recipe in scripts/nx-port-build.sh) — sudoers plugin linked STATIC (--disable-shared --enable-static-sudoers), passwd/shadow auth (no PAM), installed setuid-root (04755) in /nanos/bin, reads seeded /etc/sudoers (%wheel). Build needed many libc-glue gaps (all clean-room, committed): killpg/writev/mprotect, rlim_t+RLIMIT_*+RLIM_NLIMITS, PRIO_*, setspent/endspent/getspent, SA_SIGINFO+sa_sigaction+extended siginfo_t(si_pid/uid/status), SOMAXCONN, ifconf ifc_len, IN6_IS_ADDR_*, WIFCONTINUED, renameat decl, _PATH_VI; nx-getopt-import shim + ac_cv_func_getopt_long=yes (use system getopt, else its compat/getopt.h clashes); nx-cache killpg=yes. **FIXED + WORKING (QEMU-verified `sudo id`→uid=0(root), `sudo whoami`→root).** The "unable to
change to root gid: Not owner" was the **(uid_t)-1 sentinel bug**: picolibc uid_t is 16-bit so
NO_UID=65535 (not 0xFFFFFFFF); the libc setres*/setre*/setfs* wrappers (int)-cast it to 65535 (a
real id) while the kernel treats only -1 as "unchanged" → sudo's setresuid/setresgid(NO_UID,...)
changed ids to 65535 → EPERM. (su worked: it uses plain setuid/setgid, no sentinel.) Fix: idarg()
in syscalls.c maps (uid_t)-1→-1. Plus 3 more: (2) /run tmpfs (Kernel.cpp) for sudo's timestamp dir;
(3) sudoers `Defaults !use_pty` (NanOS can't allocate sudo's command pty — "unable to allocate pty")
+ `secure_path`; (4) **.nxe exec fallback** (Syscalls::stat/access + Exec::execve retry path+".nxe"
when absent) so bare `sudo id`/`sudo vim` resolve id.nxe. **OPEN:** %wheel is NOPASSWD — sudo's
interactive password prompt (tgetpass on /dev/tty) echoes the password and doesn't complete in the
headless console (a /dev/tty read issue; may be fine in the GUI terminal). The earlier sudo_edit.c
"conflicting types" only blocks a *rebuild* of sudo; the binary is fine and binds the fixed libc.ndl
at runtime.

**bash IS the login shell now:** GNU bash 5.2 (`make ARCH=x86_64 bash`, fork ~/Projects/bash-nanos)
is staged into the image; jan's pw_shell = /disks/main/apps/bash/bash.nxe (the BUNDLE real path,
NOT /bin/bash.nxe — execve doesn't follow a final-component symlink), root = nsh. Per-user shells,
QEMU-verified (login jan -> 'bash.nxe-5.2$'). My permission-work libc-glue changes had broken the
bash build — fixed: removed global `#include <string.h>` from compat-decls.h (broke bash's
autoconf undeclared-builtins check; moved to sudo CPPFLAGS), removed `int dprintf` decl (clashes
with bash's own void dprintf), added setres*/setfs* decls (libc.ndl now exports them so configure
uses them). bash fork's nanos/config.cache pins HAVE_TERMIOS_H/tcgetattr (else picks sgtty.h).

**Shell selection (Linux-style):** login shell = /etc/passwd field 7 (login/su honor it). `chsh`
is IN-TREE (user/chsh.c, in X64_SYS_PROGS, installed setuid-root 04755) — validates against
/etc/shells, non-root restricted to own entry, writes config/passwd (persistent) + /etc/passwd
(live). toybox's pending chsh breaks its flag generator ("Error in B" in mkflags) so it is NOT
used. /etc/shells (config/etc/shells) lists the real NanOS shells. QEMU-verified.

**App-bundle binaries need +x under DAC (3b0c589):** the /apps/<name>/<name>.nxe external
bundles (vim/doom/netsurf) + the i686 APP_PROGS/bash loops were `debugfs write`n at mode 0644 and
never chmod'd +x. With VFS DAC enforcing the exec bit, checkExec FOLLOWS the /bin/<name>.nxe
symlink to the real binary (statNoCheck follows) and denies it → `vim: Permission denied`; sudo's
access(X_OK) over secure_path skips it → `sudo: vim: command not found`. Fix: `set_inode_field
… mode 0100755` on every bundle binary in BOTH image targets (image64 + i686 _image). Rule: any
binary written into the image via debugfs needs an explicit 0100755 or DAC refuses to exec it.
Also fixed vim `E1187: Failed to source defaults.vim` once it could launch — the console flow
(init→login→bash) resets the env so PID 1's VIMRUNTIME is dropped; now exported from /etc/profile
(+ `Defaults env_keep += "VIMRUNTIME VIMINIT"` in sudoers to survive sudo's env_reset). QEMU-verified:
`vim` and `sudo vim` both open cleanly in the console.

See [[no-claude-attribution-in-commits]].

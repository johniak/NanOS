---
name: nanos-bash-port
description: "GNU bash 5.2 runs on NanOS — built from an external fork, installed as the /apps/bash bundle"
metadata: 
  node_type: memory
  type: project
  originSessionId: 661d85b4-63ba-4deb-8adc-c850a1d3125e
---

GNU bash 5.2 is ported and runs on NanOS (verified 2026-06-10): prompt, builtins,
`$()`, `$(())`, variables, pipes (fork+exec+dup2), redirection, functions, C-style
loops, and job control (^Z/jobs/fg/^C) on a pty via nterm — zero CPU faults.

**Split (user decision):** bash sources + build scaffolding live in a SEPARATE fork
repo `~/Projects/bash-nanos` (remote = upstream savannah; commit locally, never push).
NanOS only COPIES the resulting `bash.nxe`. Fork's `nanos/`: `nx-gcc` (CC wrapper —
picolibc + libc-glue headers, links like a .nxe, skips the dllimport shim for autoconf
`conftest.c` so gcc14 prototype conflicts don't break libc function probes; uses
`-fcommon`), `build.sh`, `config.cache` (cross seed; `ac_cv_func_select=yes`). bash.nxe
is gitignored.

**NanOS side:** `make bash` (optional, errors clearly if fork absent; never breaks
`make image`) builds in the container with both repos bind-mounted, stages `bin/bash.nxe`;
`_image` installs it as `/apps/bash/bash.nxe` + `/bin/bash.nxe` link only if present.

**Key enabling changes (committed on `dockerized-build`):** libc.ndl is now a COMPLETE C
library — export-all picolibc via an nm-generated `--undefined` list (minus glue), not a
curated subset. This forced two fixes: the dynamic loader's `SymTable` cap was 512/16KB and
silently dropped exports past it (PID 1 failed to load) → raised to 2048/32KB [[nanos-dll-dynamic-linking]];
and the PS/2 keyboard decoder had NO Shift state (every `$ | > *` and uppercase was
unreachable) → added Shift to `kernel/KeyDecoder`. libc gaps filled in `user/libc-glue/`
(posixstubs, pwd_grp with non-NULL pw_dir/pw_shell, sigaction/select/getentropy glue,
full termios). Phases 1a–1c (fcntl FD_CLOEXEC, sigaction, /dev/tty) committed earlier.

**Bare-name commands:** bash execs `cat`, NanOS binaries are `cat.nxe`. Solved by a fork
patch in `findcmd.c` `find_in_path_element` — a `.nxe` PATHEXT-style fallback in PATH search,
so `ls`/`cat` run by name and pipe together. Loader is extension-agnostic (checks NxHeader
magic), so no image change needed.

**Default shell (Linux-style):** account database at `/disks/main/nanos/config/passwd` —
NanOS keeps system config under `/nanos/config`, NOT `/etc` (don't reinvent `/etc`; see
docs/filesystem.md). Linux `name:passwd:uid:gid:gecos:home:shell` format; 7th field is the
login shell — edit it to set the default (read-only-disk `chsh`). Committed source is
`config/passwd`, written into the image by `_image`. `getpwuid`/`getpwnam`
(`user/libc-glue/pwd_grp.c`) parse it (fallback to built-in root). init (PID 1) and nterm
launch `getpwuid()->pw_shell`, export `$SHELL` + default `$PATH`, strip `.nxe` from argv[0]
(prompt reads `bash`), fall back to nsh if the shell is absent. **Default ships as bash.**

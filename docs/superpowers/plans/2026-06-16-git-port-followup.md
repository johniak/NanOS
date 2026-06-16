# git port — status & follow-up (DO POPRAWY)

GNU **git 2.54.0** is ported to NanOS via the i686-nanos SDK as the "real `-lpthread` + `fork()`
application" gate for the pthread work. It **builds, loads, runs, and commits** on NanOS — but the
`git gc` path is **not yet complete**. This file records exactly what works, what's left, and how
to reproduce, so the remaining work can be picked up cleanly.

## Where it lives
- Build source + manifest: `~/Projects/nanos-sdk-work/git-port/` (`nxport.toml`, `git-2.54.0/`).
  `git-2.54.0/` is a **copy** of upstream git 2.54.0 with a `config.mak` (build knobs) and two
  small source patches in `run-command.c` (see below). NOTE: `~/Projects/git` is a *separate*
  upstream clone and is NOT the build source — edit `git-port/git-2.54.0/`.
- Build: `make git` (refreshes the SDK sysroot from this checkout, runs `nanos-port` in the
  `nanos-sdk-dev` container, stages `bin/git.nxe`) then `make image` installs it.
- Install layout: `/nanos/bin/git.nxe` (run `git` by name) + `/nanos/libexec/git-core/git.nxe`
  and `git-<cmd>.nxe` hardlinks (git's exec-path; `run-command` execs subcommands from here).

## WORKS (QEMU-verified)
A real git workflow on the ext4 disk, run from the **`cd`** form (see the `git -C` caveat below):
```
git init /disks/main/repo
cd /disks/main/repo
git config user.name t ; git config user.email t ; git config pack.threads 4
echo hi > a.txt
git add a.txt
git commit -m first        ->  [master (root-commit) <sha>] first / 1 file changed, 1 insertion(+)
```
This exercises the full gate: git is built **with pthreads** (import table has
`pthread_create/join/mutex`), and it **forks + execs** subcommands (`maintenance` after commit) via
`run-command` (import table has `fork/execve/waitpid/pipe`). `git commit` completes cleanly (0 CPU
faults), writing real objects/refs to ext4. `git rev-parse`, `git log` (read) also work.

## Bugs found AND FIXED on the way here (all landed in this repo / the port)
1. **errno ABI** — git compiled against picolibc's `extern int errno` (data) but runtime libc.ndl
   exports only `__errno_location` (function, per the per-thread-errno migration). Fix:
   `-D__PICOLIBC_ERRNO_FUNCTION=__errno_location` in `git-port/git-2.54.0/config.mak`.
2. **stale sysroot crt0** — `make git`/ping/wget refreshed libc.ndl but NOT crt0.o, so the SDK
   sysroot had a pre-migration crt0 without the `__nx_init_tls` main-thread TLS bootstrap → the
   first `errno` access faulted (`cr2=0x1c`). Fix: `make git` now also copies `bin/crt0.o` +
   `bin/nxhdr.o` into the sysroot (Makefile). Benefits ALL nanos-port ports.
3. **exec `.nxe` convention** — NanOS programs are `<name>.nxe`; git execs bare `git-<cmd>` names
   and pre-checks them via `is_executable`/`locate_in_PATH`. Fixes: (a) libc `execve` retries with
   `.nxe` on ENOENT (`user/libc-glue/syscalls.c`); (b) two small patches in
   `git-port/git-2.54.0/run-command.c` — `is_executable` accepts a regular `.nxe` file (NanOS ext
   carries no exec bit), and `locate_in_PATH` retries each PATH entry with `.nxe`; (c) git-core
   binaries installed as `.nxe` hardlinks. Result: git subcommands now **fork+exec successfully**.
4. **getdelim** — picolibc's `getdelim` returned 0 at EOF; POSIX requires -1. git's
   `strbuf_getwholeline` does `assert(r == -1)`, so subprocesses aborted (SIGABRT, "died of signal
   6"). Fix: correct `getdelim`/`getline` in `user/libc-glue/getdelim.c` (overrides picolibc).
   After this, `git commit` is fully clean and `maintenance`/`reflog` no longer abort.

## TO FIX (remaining)
1. **`git gc` → repack↔pack-objects pipe.** `git gc` forks `repack`, which forks `pack-objects`
   (the threaded delta search — `pack.threads`) and reads the resulting pack object-ids from
   pack-objects' stdout over a pipe. It fails with:
   `fatal: repack: Expecting full hex object ID lines only from pack-objects.`
   pack-objects **runs** (0 faults, threads compiled in), but repack cannot parse its piped
   stdout. Likely a NanOS pipe/dup2 plumbing or a pack-objects stdout-format issue under the
   bidirectional `run-command` pipe. This is the last blocker to seeing pack-objects' threads do
   real work end-to-end. Start at git `builtin/repack.c` (`finish_pack_objects_cmd` /
   `read_packs_from_stdin`) + NanOS `kernel/` pipe + the `start_command` dup2 path.
2. **`git -C <path>` cwd quirk.** `git -C /disks/main/repo config` resolves its lockfile against
   cwd `/` (the read-only SynthFs root) → `EROFS` on `/.git/config.lock`, even though our `chdir`
   and `getcwd` are correct (verified: `cd` + bare `git` works; a kernel chdir probe showed git
   chdirs to the repo exactly once with no revert; `git rev-parse --absolute-git-dir` is correct).
   It is a git-internal startup-cwd/path interaction, not a NanOS kernel bug. **Workaround: use
   `cd <repo>` instead of `git -C <repo>`.** Worth chasing in git's `setup.c`/lockfile if `-C`
   support is wanted.

## Limits (by design, not bugs)
- No external helpers: `less` (pager), and anything git would `exec` that isn't a git builtin, are
  absent → "cannot run less" etc. Use `git --no-pager` / builtins only.
- `git init` warns "templates not found in /nanos/share/git-core/templates" (harmless; the repo is
  still initialized). Ship a minimal template dir if the warning is undesirable.
- HTTP/S transport is disabled (`NO_CURL`); local repositories only.

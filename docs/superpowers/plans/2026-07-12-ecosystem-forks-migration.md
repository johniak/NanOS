# NanOS-labs Ecosystem Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Everything NanOS-related on this Mac becomes a tracked repo under `github.com/NanOS-labs`, with port recipes inside each fork, a manifest-driven `bootstrap.sh`, `docs/ECOSYSTEM.md`, and a clean-room reproducibility gate.

**Architecture:** Two scripts in the `nanos-sdk` repo do the heavy lifting: `scripts/migrate-fork.sh` (one port → one org repo: pristine base commit + audited in-place-modification commit + recipe commit) driven by `ports.manifest`, and `scripts/bootstrap.sh` (fresh machine → clones every fork into today's `nanos-sdk-work/` layout → toolchain → docker images → ordered port builds → `image64`). Fork repos mirror the on-disk layout (`portdir` = recipe at root + source subdir; `srcroot` = source at root) so **zero NanOS Makefile paths change**.

**Tech Stack:** bash, git, gh CLI (authed as johniak, org NanOS-labs), Docker, existing NanOS/nanos-sdk make targets.

**Spec:** `docs/superpowers/specs/2026-07-12-ecosystem-reproducibility-design.md`

## Global Constraints

- Org: `github.com/NanOS-labs`. All new repos `--public` (match existing johniak repos), default branch `nanos` (own-code repos: `main`).
- Commits/PRs: **no Claude/AI attribution trailers** (user rule).
- On recipe drift, **the `~/Projects/nanos-sdk-work` version wins** (it is what builds today).
- No NanOS `Makefile` path changes in this plan — bootstrap recreates today's `$(SDK_WORK)` layout exactly.
- Every fork migration must show its post-overlay `git status --short` (the audit that no in-place modification is silently lost). Empty audit = clean upstream; non-empty = must be inspected before commit.
- `verify64` must be green at the end (nothing in this plan touches kernel/userland code, but the gate confirms it).
- Work directory for repo assembly: `~/Projects/nanos-sdk-work/_migrate/` (gitignored by nothing — it's inside the unversioned workspace; deleted at the end).

---

### Task 1: Rescue `nano-packages` (no remote — highest risk first)

**Files:**
- Modify: `~/Projects/nano-packages/.gitignore` (create if absent)

**Interfaces:**
- Produces: `github.com/NanOS-labs/nano-packages` containing the existing 28 commits.

- [ ] **Step 1: Clean runtime junk out of the tree**

```bash
cd ~/Projects/nano-packages
printf '%s\n' 'server/db.sqlite3' 'server/.coverage' 'server/media/' '__pycache__/' '*.pyc' 'client/target/' >> .gitignore
git rm --cached server/db.sqlite3 2>/dev/null; git rm --cached server/.coverage 2>/dev/null
git add .gitignore && git commit -m "chore: gitignore runtime artifacts before first push"
```

Expected: one commit; `git status --porcelain` shows nothing (db.sqlite3 now ignored).

- [ ] **Step 2: Create the org repo and push**

```bash
cd ~/Projects/nano-packages
gh repo create NanOS-labs/nano-packages --public --description "nap - the NanOS package manager (Rust client + registry server)" 
git remote add origin git@github.com:NanOS-labs/nano-packages.git
git push -u origin HEAD
```

Expected: push succeeds; `gh repo view NanOS-labs/nano-packages --json defaultBranchRef -q .defaultBranchRef.name` prints the pushed branch.

- [ ] **Step 3: Verify all 29 commits are remote**

```bash
cd ~/Projects/nano-packages && git log --oneline origin/HEAD 2>/dev/null | wc -l; git status -sb | head -1
```

Expected: count ≥ 29 and branch shows `[origin/...]` tracking with no `ahead`.

### Task 2: Rescue `sqlite-nanos`

- [ ] **Step 1: Create org repo and push**

```bash
cd ~/Projects/sqlite-nanos
git log --oneline | head -3   # sanity: repo has commits
gh repo create NanOS-labs/sqlite-nanos --public --description "SQLite port to NanOS (x86_64)"
git remote add origin git@github.com:NanOS-labs/sqlite-nanos.git
git push -u origin HEAD
```

Expected: push succeeds, `git status -sb` shows tracking, no `ahead`.

### Task 3: Rescue the `nanoos-ui` mockup + untracked docs into NanOS

**Files:**
- Create: `docs/design/nanoos-ui/index.html`, `docs/design/nanoos-ui/README.md` (from `~/Downloads/nanoos-ui(1).zip`)
- Add (already exist, untracked): `docs/superpowers/plans/2026-07-04-ext-path-ub-kernel-o2.md`, `docs/superpowers/plans/2026-07-04-tech-debt-priorities.md`, `docs/superpowers/plans/2026-07-03-aero-liquid-glass-frames.html`, `docs/superpowers/plans/2026-07-12-rsexp-files-redesign.html`, `docs/superpowers/plans/README.md` (modified)

- [ ] **Step 1: Unzip the mockup into docs/design**

```bash
cd ~/Projects/NanOS
mkdir -p docs/design
unzip -o ~/Downloads/"nanoos-ui(1).zip" -d docs/design/
ls docs/design/nanoos-ui/    # expect index.html README.md
```

- [ ] **Step 2: Commit mockup + the untracked plan docs**

```bash
cd ~/Projects/NanOS
git add docs/design/nanoos-ui docs/superpowers/plans/2026-07-04-ext-path-ub-kernel-o2.md docs/superpowers/plans/2026-07-04-tech-debt-priorities.md docs/superpowers/plans/2026-07-03-aero-liquid-glass-frames.html docs/superpowers/plans/2026-07-12-rsexp-files-redesign.html docs/superpowers/plans/README.md
git commit -m "docs: rescue nanoos-ui design mockup + commit loose plan docs"
```

Note: `nanos-files-explorer-design.html` (repo root) duplicates `2026-07-12-rsexp-files-redesign.html` — check with `diff`; if identical, delete the root copy instead of committing it. `docs/superpowers/plans/2026-07-07-electron-marktext/` is a scratch app checkout, NOT docs — leave untracked. `disk/image64-grub2.img`, `scratch/` stay untracked.

- [ ] **Step 3: Verify git status is down to expected noise**

```bash
git status --porcelain   # expect only: disk/image64-grub2.img, scratch/, 2026-07-07-electron-marktext/, possibly root html if kept
```

### Task 4: Transfer the six existing repos into NanOS-labs

**Interfaces:**
- Produces: `NanOS-labs/{NanOS,nanos-sdk,vim-nanos,ncurses-nanos,bash-nanos,netsurf-nanos}`; local remotes updated. GitHub leaves redirects at the old URLs.

- [ ] **Step 1: Transfer via API (repo by repo)**

```bash
for r in NanOS nanos-sdk vim-nanos ncurses-nanos bash-nanos netsurf-nanos; do
  gh api -X POST "repos/johniak/$r/transfer" -f new_owner=NanOS-labs --jq .full_name
done
```

Expected: six lines `NanOS-labs/<name>` (transfer to an org you own completes immediately).

- [ ] **Step 2: Update local remotes**

```bash
cd ~/Projects/NanOS         && git remote set-url origin git@github.com:NanOS-labs/NanOS.git
cd ~/Projects/nanos-sdk     && git remote set-url origin git@github.com:NanOS-labs/nanos-sdk.git
cd ~/Projects/vim-nanos     && git remote set-url origin git@github.com:NanOS-labs/vim-nanos.git
cd ~/Projects/ncurses-nanos && git remote set-url origin git@github.com:NanOS-labs/ncurses-nanos.git
cd ~/Projects/bash-nanos    && git remote set-url github git@github.com:NanOS-labs/bash-nanos.git
cd ~/Projects/netsurf-nanos && git remote set-url origin git@github.com:NanOS-labs/netsurf-nanos.git
for d in NanOS nanos-sdk vim-nanos ncurses-nanos bash-nanos netsurf-nanos; do cd ~/Projects/$d && git fetch --all -q && echo "$d OK"; done
```

Expected: six `OK` lines.

- [ ] **Step 3: Fix hardcoded URLs**

```bash
cd ~/Projects/NanOS && grep -rn "github.com/johniak" --include="*.md" --include="*.sh" --include="Makefile" . | grep -v scratch/
cd ~/Projects/nanos-sdk && grep -rn "github.com/johniak" .
```

Replace every hit with `github.com/NanOS-labs/...` (Edit each file), then commit per repo: `git commit -am "docs: repo URLs moved to the NanOS-labs org"`. If grep finds nothing, skip the commit.

- [ ] **Step 4: Push NanOS pending commits**

```bash
cd ~/Projects/NanOS && git push origin develop
```

### Task 5: Manifest + `migrate-fork.sh` in nanos-sdk

**Files:**
- Create: `~/Projects/nanos-sdk/ports.manifest`
- Create: `~/Projects/nanos-sdk/scripts/migrate-fork.sh`

**Interfaces:**
- Produces: `migrate-fork.sh <name>` — reads the manifest row, assembles the repo in `$SDK_WORK/_migrate/<name>`, creates `NanOS-labs/<repo>`, pushes branch `nanos` (default). `bootstrap.sh` (Task 12) consumes the same manifest.
- Manifest columns (|-separated): `name|repo|layout|base|checkout|desc`
  - `layout`: `portdir` (repo root = the `*-port` dir, source in subdir) | `srcroot` (repo root = source tree) | `multi` (repo root holds several port dirs)
  - `base`: `tarball:<path-relative-to-SDK_WORK>` (pristine base from tarball, then overlay audit) | `tree` (commit local tree as base; provenance note)
  - `checkout`: where bootstrap clones it, relative to `$SDK_WORK`

- [ ] **Step 1: Write `ports.manifest`**

```
# name|repo|layout|base|checkout|desc
zlib|zlib-nanos|portdir|tree|zlib-port|zlib for NanOS + port recipe
openssl|openssl-nanos|portdir|tree|openssl-port|OpenSSL 3.0.15 port (CSPRNG/TLS)
dropbear|dropbear-nanos|portdir|tree|dropbear-port|Dropbear SSH server port
git|git-nanos|portdir|tree|git-port|git 2.54 port
htop|htop-nanos|portdir|tree|htop-port|htop 3.5.1 port
wget|wget-nanos|portdir|tree|wget-port|wget 1.21.4 port (TLS client)
libpng|libpng-nanos|portdir|tree|libpng-port|libpng 1.6.43 port
libjpeg|libjpeg-nanos|portdir|tree|libjpeg-port|libjpeg port
darkhttpd|darkhttpd-nanos|portdir|tree|darkhttpd-port|darkhttpd web server port
ncprobe|ncprobe|portdir|tree|ncprobe-port|ncprobe - NanOS-native network console probe (own code)
inetutils|inetutils-nanos|multi|tree|inetutils-port|GNU inetutils 2.5 ports (ping + services)
toybox|toybox-nanos|srcroot|tarball:toybox-0.8.11.tar.gz|toybox-0.8.11|toybox 0.8.11 (login/su/passwd/id)
sudo|sudo-nanos|srcroot|tarball:sudo-1.9.15p5.tar.gz|sudo-1.9.15p5|sudo 1.9.15p5
grep|grep-nanos|srcroot|tarball:grep-3.11.tar.xz|grep-3.11|GNU grep 3.11
bzip2|bzip2-nanos|srcroot|tarball:bzip2.tgz|bzip2-1.0.8|bzip2 1.0.8
busybox|busybox-nanos|srcroot|tarball:busybox-1.36.1.tar.bz2|busybox-1.36.1|busybox 1.36.1 (udhcpc)
libdrm|libdrm-nanos|portdir|tree|libdrm-port|libdrm 2.4.123 + NanOS patch + recipe
mesa|mesa-nanos|portdir|tree|mesa-port|Mesa 24.2.8 + 4 NanOS patches + GL build recipes (nwm-gl/glkms/gles2info)
qemu|qemu-nanos|portdir|tree|qemu-fork-build|QEMU host fork - macOS virgl 3D-scanout fixes
virglrenderer|virglrenderer-nanos|portdir|tree|virgl-fork-build|virglrenderer 1.3.0 + vendored kosmickrisp patches + drain branch
```

Save as `~/Projects/nanos-sdk/ports.manifest`. Note `base=tree` for portdir repos: the pristine source is either already patch-tracked (mesa/libdrm patches exist as files) or has no local tarball; the audit for these is Step 4 of Task 6. `bzip2.tgz` contains `bzip2-1.0.8/`.

- [ ] **Step 2: Write `scripts/migrate-fork.sh`**

```bash
#!/usr/bin/env bash
# migrate-fork.sh <manifest-name> [--dry-run]
# Assembles one NanOS-labs repo from the nanos-sdk-work workspace per ports.manifest:
#   base commit (pristine tarball or local tree, provenance in the message)
#   -> overlay commit (in-place modifications, AUDITED: prints git status before committing)
#   -> push branch 'nanos' as the org repo default.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
SDK_WORK="${SDK_WORK:-$HOME/Projects/nanos-sdk-work}"
ORG=NanOS-labs
NAME="${1:?usage: migrate-fork.sh <name> [--dry-run]}"
DRY="${2:-}"

row=$(grep -v '^#' "$HERE/ports.manifest" | awk -F'|' -v n="$NAME" '$1==n{print; exit}')
[ -n "$row" ] || { echo "no manifest row for '$NAME'"; exit 1; }
IFS='|' read -r _ REPO LAYOUT BASE CHECKOUT DESC <<<"$row"

WORK="$SDK_WORK/_migrate/$REPO"
rm -rf "$WORK"; mkdir -p "$WORK"; cd "$WORK"
git init -q -b nanos

# .gitignore FIRST so build junk never enters history (sources never match these).
cat > .gitignore <<'EOF'
build/
install/
*.o
*.a
*.so
*.dylib
*.nxe
*.elf
*.ndl
*.log
.cache/
__pycache__/
*.pyc
target/
.DS_Store
EOF
git add .gitignore && git commit -qm "chore: ignore build artifacts"

# 1) base
case "$BASE" in
  tarball:*)
    TB="$SDK_WORK/${BASE#tarball:}"
    [ -f "$TB" ] || { echo "missing tarball $TB"; exit 1; }
    tar xf "$TB" --strip-components=1
    git add -A && git commit -qm "upstream base: $(basename "$TB") (pristine)

Extracted verbatim from the tarball archived in nanos-sdk-work."
    git tag upstream-base
    ;;
  tree)
    : # base IS the local tree; the overlay step below makes the single base commit
    ;;
  *) echo "unknown base kind '$BASE'"; exit 1;;
esac

# 2) overlay the live workspace tree (rsync keeps .git and .gitignore)
case "$LAYOUT" in
  portdir|multi) SRC="$SDK_WORK/$CHECKOUT";;
  srcroot)       SRC="$SDK_WORK/$CHECKOUT";;
esac
[ -d "$SRC" ] || { echo "missing source tree $SRC"; exit 1; }
rsync -a --delete --exclude .git --exclude .gitignore "$SRC/" "$WORK/"
if [ "$NAME" = inetutils ]; then   # multi: fold the sibling services port dir in
  mkdir -p inetutils-services-port
  rsync -a --exclude .git "$SDK_WORK/inetutils-services-port/" "$WORK/inetutils-services-port/"
fi

echo "===== AUDIT: changes vs base for $NAME ====="
git add -A
git status --short | head -100
echo "===== (empty above = tree is pristine upstream) ====="

if [ "$DRY" = "--dry-run" ]; then echo "dry-run: stopping before commit/push"; exit 0; fi

if ! git diff --cached --quiet; then
  if [ "$BASE" = tree ]; then
    git commit -qm "import: $NAME workspace tree from nanos-sdk-work

Base = the live working tree (no pristine local tarball). Upstream
provenance and applied patches are documented in-repo and in
NanOS docs/ECOSYSTEM.md."
  else
    git commit -qm "nanos: in-place modifications carried from nanos-sdk-work

Everything the AUDIT block above listed vs the pristine base."
  fi
fi

# 3) create org repo + push
gh repo create "$ORG/$REPO" --public --description "$DESC" >/dev/null
git remote add origin "git@github.com:$ORG/$REPO.git"
git push -q -u origin nanos --tags
gh api -X PATCH "repos/$ORG/$REPO" -f default_branch=nanos >/dev/null
echo "DONE: https://github.com/$ORG/$REPO"
```

```bash
chmod +x ~/Projects/nanos-sdk/scripts/migrate-fork.sh
```

- [ ] **Step 3: Shellcheck it**

Run: `shellcheck ~/Projects/nanos-sdk/scripts/migrate-fork.sh` (brew install shellcheck if absent)
Expected: no errors (style warnings acceptable).

- [ ] **Step 4: Dry-run on zlib and inspect the audit**

```bash
~/Projects/nanos-sdk/scripts/migrate-fork.sh zlib --dry-run
```

Expected: `AUDIT` block lists the zlib-port files (`nxport.toml`, `src/…`); no push happens. Confirm nothing unexpected (e.g. stray `.orig` files).

- [ ] **Step 5: Commit manifest + script to nanos-sdk**

```bash
cd ~/Projects/nanos-sdk
git add ports.manifest scripts/migrate-fork.sh
git commit -m "ports: manifest + migrate-fork.sh - assemble NanOS-labs forks from the sdk-work workspace"
git push
```

### Task 6: Migrate the simple ports (15 repos)

**Interfaces:**
- Consumes: `migrate-fork.sh` (Task 5).
- Produces: 15 org repos matching the manifest.

- [ ] **Step 1: Tarball-based srcroot forks (real pristine-base audit)**

```bash
cd ~/Projects/nanos-sdk
for n in toybox sudo grep bzip2 busybox; do scripts/migrate-fork.sh $n; done
```

Expected per port: AUDIT block (INSPECT EACH — e.g. toybox and sudo are expected to show NanOS build scripts/config; any modified upstream `.c` is a finding to keep, which the commit does automatically), then `DONE: https://github.com/NanOS-labs/<repo>`.

- [ ] **Step 2: Recipe portdir forks**

```bash
cd ~/Projects/nanos-sdk
for n in zlib openssl dropbear git htop wget libpng libjpeg darkhttpd ncprobe inetutils; do scripts/migrate-fork.sh $n; done
```

Expected: 11 `DONE` lines. `base=tree`, so each repo's first content commit is the whole port dir (recipe + source tree, artifacts excluded by .gitignore).

- [ ] **Step 3: Spot-verify on GitHub**

```bash
gh repo list NanOS-labs --limit 40
gh api repos/NanOS-labs/git-nanos/contents/nxport.toml --jq .name
gh api repos/NanOS-labs/toybox-nanos --jq .default_branch
```

Expected: repo list shows all; `nxport.toml`; `nanos`.

### Task 7: Deep-audit the tree-based ports against pristine upstream

The `base=tree` forks are tracked now, but the spec requires proving what differs from upstream. Do it post-hoc, recorded in each repo.

- [ ] **Step 1: Fetch pristine upstreams and diff (per port, scripted)**

```bash
mkdir -p ~/Projects/nanos-sdk-work/_migrate/pristine && cd ~/Projects/nanos-sdk-work/_migrate/pristine
# canonical tarballs — versions read from the trees during migration
curl -LO https://www.openssl.org/source/openssl-3.0.15.tar.gz
curl -LO https://mirrors.edge.kernel.org/pub/software/scm/git/git-2.54.0.tar.xz
curl -LO https://ftp.gnu.org/gnu/wget/wget-1.21.4.tar.gz
curl -LO https://ftp.gnu.org/gnu/inetutils/inetutils-2.5.tar.xz
curl -LO https://download.sourceforge.net/libpng/libpng-1.6.43.tar.xz
curl -LO https://dri.freedesktop.org/libdrm/libdrm-2.4.123.tar.xz
curl -LO https://archive.mesa3d.org/mesa-24.2.8.tar.xz
```

For each: extract next to the fork checkout and `diff -rq --exclude=.git --exclude=build <pristine> <fork-src-subdir>`; save the output as `docs/UPSTREAM-DIFF.md` in that fork (`git add docs/UPSTREAM-DIFF.md && git commit -m "docs: recorded diff vs pristine upstream" && git push`). Ports whose upstream tarball URL is dead or version ambiguous (libjpeg, dropbear, htop, zlib, darkhttpd, ncurses source in ncurses-nanos): record the closest identification (`grep`-derived version + source URL) in the same file with a note that base = imported tree.

Expected: every `*-nanos` repo with `base=tree` gains a committed `docs/UPSTREAM-DIFF.md`.

- [ ] **Step 2: Audit musl + picolibc trees (fork only if dirty)**

```bash
cd ~/Projects/nanos-sdk-work/_migrate/pristine
curl -LO https://musl.libc.org/releases/musl-1.2.5.tar.gz && tar xf musl-1.2.5.tar.gz
diff -rq --exclude=.git musl-1.2.5 ~/Projects/nanos-sdk-work/musl-1.2.5 | grep -v "^Only in musl-1.2.5" | head -30
cd ~/Projects/nanos-sdk-work/picolibc && git status --porcelain 2>/dev/null | head || echo "picolibc: not a git repo - diff against the version nanos-sdk/build-toolchain.sh pins"
```

If musl/picolibc show real source diffs → run them through `migrate-fork.sh` too (add manifest rows `musl|musl-nanos|srcroot|tree|musl-1.2.5|…`). If clean → note "verified pristine, materialized by bootstrap from upstream" in ECOSYSTEM.md (Task 13). Either way the finding is recorded.

- [ ] **Step 3: cxx-port recipe into nanos-sdk**

```bash
cd ~/Projects/nanos-sdk
mkdir -p ports/cxx && cp ~/Projects/nanos-sdk-work/cxx-port/build-libstdcxx.sh ports/cxx/
git add ports/cxx && git commit -m "ports: libstdc++ build recipe (cxx-port) - gcc tree comes from the toolchain patch flow" && git push
```

(The gcc-14.2.0 tree inside cxx-port is regenerated by `patch.sh` + tarball; only the recipe needs a home.)

### Task 8: Reconcile drifted recipes into vim-nanos / ncurses-nanos

- [ ] **Step 1: vim-nanos — sdk-work recipe wins**

```bash
diff -ru ~/Projects/vim-nanos/nxport.toml ~/Projects/nanos-sdk-work/vim-port/nxport.toml
diff -ru ~/Projects/vim-nanos/hooks ~/Projects/nanos-sdk-work/vim-port/hooks 2>/dev/null
cp ~/Projects/nanos-sdk-work/vim-port/nxport.toml ~/Projects/vim-nanos/nxport.toml
cp -R ~/Projects/nanos-sdk-work/vim-port/hooks/. ~/Projects/vim-nanos/hooks/ 2>/dev/null || true
cp ~/Projects/nanos-sdk-work/vim-port/vim.install ~/Projects/vim-nanos/ 2>/dev/null || true
cd ~/Projects/vim-nanos && git add -A && git commit -m "nanos: sync port recipe from the live nanos-sdk-work copy (drift reconciliation)" && git push
```

- [ ] **Step 2: ncurses-nanos — same procedure**

```bash
diff -ru ~/Projects/ncurses-nanos/nxport.toml ~/Projects/nanos-sdk-work/ncurses-port/nxport.toml 2>/dev/null
cp ~/Projects/nanos-sdk-work/ncurses-port/nxport.toml ~/Projects/ncurses-nanos/ 2>/dev/null || true
cp -R ~/Projects/nanos-sdk-work/ncurses-port/hooks/. ~/Projects/ncurses-nanos/hooks/ 2>/dev/null || true
cd ~/Projects/ncurses-nanos && git add -A && git diff --cached --quiet || { git commit -m "nanos: sync port recipe from the live nanos-sdk-work copy"; git push; }
```

Also check bash-nanos and netsurf-nanos the same way against any recipe copies in sdk-work (`ls ~/Projects/nanos-sdk-work | grep -i 'bash\|netsurf'` — none found in the audit, so expect no-op).

### Task 9: mesa-nanos + libdrm-nanos

- [ ] **Step 1: Migrate both via the script**

```bash
cd ~/Projects/nanos-sdk
scripts/migrate-fork.sh libdrm
scripts/migrate-fork.sh mesa
```

Inspect the mesa AUDIT block carefully — the mesa-24.2.8 subtree carries the 4 patches applied in place; the patch files ride along in `patches/`, both is correct and intended. Expected: 2 `DONE` lines. (mesa repo will be large, ~200MB — fine.)

- [ ] **Step 2: Cross-check the 4 mesa patches are represented**

```bash
gh api repos/NanOS-labs/mesa-nanos/contents/patches --jq '.[].name'
```

Expected: the 4 `000?-*.patch` names.

### Task 10: qemu-nanos (host fork, commit-per-fix history)

This one is NOT script-migrated — the spec wants the 3 fixes as reviewable commits on the pristine base.

- [ ] **Step 1: Assemble base from the archived tarball**

```bash
W=~/Projects/nanos-sdk-work/_migrate/qemu-nanos; rm -rf $W; mkdir -p $W; cd $W
git init -q -b nanos
tar xf ~/Projects/nanos-sdk-work/qemu-fork-build/qemu.tar.gz --strip-components=1
printf 'build/\ninstall/\n.DS_Store\n' > .gitignore
git add -A && git commit -qm "upstream base: QEMU master snapshot (qemu.tar.gz from nanos-sdk-work)

Provenance: the exact tree build-qemu.sh has been building; upstream master
already contains the texture-borrowing + cocoa ANGLE GL work."
git tag upstream-base
```

Note: the pristine tarball may already differ from the live `qemu-src` only by our 3 fixes — Step 2 verifies exactly that.

- [ ] **Step 2: Apply each fix as its own commit**

```bash
cd $W
F=~/Projects/nanos-sdk-work/qemu-fork-build/nanos-fixes
cp $F/cocoa.m.modified            ui/cocoa.m
git commit -am "nanos: cocoa GL scanout fix (macOS Metal-EGLImage path)"
cp $F/console.c.modified          ui/console.c
git commit -am "nanos: ui/console GL scanout dispatch fix"
cp $F/virtio-gpu-virgl.c.modified hw/display/virtio-gpu-virgl.c
git commit -am "nanos: virtio-gpu-virgl scanout fix"
# verify the result matches the LIVE tree the working QEMU was built from:
diff -rq --exclude=build --exclude=.git . ~/Projects/nanos-sdk-work/qemu-fork-build/qemu-src | head
```

Expected: final diff EMPTY (or only junk like .DS_Store). Non-empty source diffs = the live tree has changes beyond the 3 saved fixes → inspect and commit them too (`git add -A && git commit -m "nanos: additional live-tree changes recovered during migration"`).

- [ ] **Step 3: Add the build script + push**

```bash
cd $W
mkdir -p nanos && cp ~/Projects/nanos-sdk-work/qemu-fork-build/build-qemu.sh nanos/
git add nanos && git commit -qm "nanos: host build script (virgl+cocoa, venus off)"
gh repo create NanOS-labs/qemu-nanos --public --description "QEMU host fork for NanOS - macOS virgl 3D scanout (cocoa Metal-EGLImage)"
git remote add origin git@github.com:NanOS-labs/qemu-nanos.git
git push -q -u origin nanos --tags
gh api -X PATCH repos/NanOS-labs/qemu-nanos -f default_branch=nanos >/dev/null
git log --oneline   # expect: base + 3 fix commits + build script (+ possible recovered-changes commit)
```

### Task 11: virglrenderer-nanos (vendored tap patches + drain branch)

- [ ] **Step 1: Base + vendored kosmickrisp patches as commits**

```bash
W=~/Projects/nanos-sdk-work/_migrate/virglrenderer-nanos; rm -rf $W; mkdir -p $W; cd $W
git init -q -b nanos
tar xf ~/Projects/nanos-sdk-work/virgl-fork-build/virglrenderer.tar.gz --strip-components=1
printf 'build/\n.cache/\n.DS_Store\n__pycache__/\n' > .gitignore
git add -A && git commit -qm "upstream base: virglrenderer 1.3.0 (pristine)"
git tag upstream-base
TAP=/opt/homebrew/Library/Taps/startergo/homebrew-virglrenderer/patches
mkdir -p nanos/patches && cp "$TAP"/*.patch nanos/patches/
git add nanos/patches && git commit -qm "nanos: vendor the startergo/kosmickrisp macOS patch set (brew tap is not a durable home)"
for p in $(sed -n 's/^  \(virglrenderer-.*\.patch\)$/\1/p' ~/Projects/nanos-sdk-work/virgl-fork-build/build-virgl.sh); do
  git apply "nanos/patches/$p" && git commit -aqm "kosmickrisp: $p" || { echo "PATCH FAILED: $p"; break; }
done
git log --oneline | head -25
```

Expected: base + vendor commit + one commit per tap patch (~20), in build-virgl.sh order. A failing patch means build-virgl.sh's list/order is authoritative — fix the loop's list to match it exactly (it is the list that builds the working dylib).

- [ ] **Step 2: Verify parity with the live built tree, add build script**

```bash
cd $W
diff -rq --exclude=build --exclude=.git --exclude=.cache --exclude=nanos . ~/Projects/nanos-sdk-work/virgl-fork-build/virglrenderer-1.3.0 | head
```

Expected: empty (tap patches fully explain the live tree). Non-empty → commit the residue as `nanos: live-tree changes beyond the tap set`.

```bash
cp ~/Projects/nanos-sdk-work/virgl-fork-build/build-virgl.sh nanos/
git add nanos/build-virgl.sh && git commit -qm "nanos: host build script (reads vendored patches, not the brew tap)"
```

Then edit `nanos/build-virgl.sh`: change `PATCHDIR="$TAP/patches"` to `PATCHDIR="$HERE/nanos/patches"` so it consumes the vendored copies; commit `git commit -am "nanos: build from vendored patches"`.

- [ ] **Step 3: The drain experiment as a branch**

```bash
cd $W
git checkout -b drain
rsync -a --delete --exclude .git --exclude build --exclude .cache --exclude nanos --exclude .gitignore \
  ~/Projects/nanos-sdk-work/virgl-fork-build/virglrenderer-1.3.0-drain/ .
git add -A && git status --short | head    # AUDIT: expect src/virglrenderer.c + src/vrend/vrend_renderer.c
git commit -qm "drain: scanout drain experiment (vrend_renderer + virglrenderer)"
git checkout nanos
```

- [ ] **Step 4: Push**

```bash
cd $W
gh repo create NanOS-labs/virglrenderer-nanos --public --description "virglrenderer 1.3.0 for the NanOS host GL stack - vendored kosmickrisp macOS patches + experiments"
git remote add origin git@github.com:NanOS-labs/virglrenderer-nanos.git
git push -q -u origin nanos --tags && git push -q origin drain
gh api -X PATCH repos/NanOS-labs/virglrenderer-nanos -f default_branch=nanos >/dev/null
```

### Task 12: `bootstrap.sh` in nanos-sdk

**Files:**
- Create: `~/Projects/nanos-sdk/scripts/bootstrap.sh`

**Interfaces:**
- Consumes: `ports.manifest` (same columns as Task 5).
- Produces: a populated `$SDK_WORK` + toolchain + docker images; then prints the make commands for the ordered port builds.

- [ ] **Step 1: Write the script**

```bash
#!/usr/bin/env bash
# bootstrap.sh — fresh machine -> populated nanos-sdk-work + toolchain + docker, ready for `make image64`.
# Idempotent: every step checks its own done-marker and skips. SDK_WORK overridable (clean-room gate).
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
SDK_WORK="${SDK_WORK:-$HOME/Projects/nanos-sdk-work}"
ORG=git@github.com:NanOS-labs
step() { printf '\n== %s ==\n' "$*"; }

step "host prerequisites"
for c in git docker rsync curl python3; do command -v $c >/dev/null || { echo "MISSING: $c"; exit 1; }; done
docker info >/dev/null 2>&1 || { echo "docker daemon not running"; exit 1; }
python3 -c 'import PIL' 2>/dev/null || echo "NOTE: python3 Pillow missing (needed only for 'make assets')"
if [ "$(uname)" = Darwin ]; then
  command -v qemu-system-x86_64 >/dev/null || echo "NOTE: brew install qemu (interactive run64)"
  brew list --versions startergo/libepoxy/libepoxy startergo/angle/angle 2>/dev/null || \
    echo "NOTE: GL host stack (run64-gl) needs: brew tap startergo/{libepoxy,angle} && brew install ... + qemu-nanos/virglrenderer-nanos builds"
fi

step "clone forks into the sdk-work layout"
mkdir -p "$SDK_WORK"
grep -v '^#' "$HERE/ports.manifest" | while IFS='|' read -r name repo layout base checkout desc; do
  [ -n "$name" ] || continue
  dest="$SDK_WORK/$checkout"
  if [ -d "$dest/.git" ]; then echo "  $checkout: present"; continue; fi
  echo "  cloning $repo -> $checkout"
  git clone -q "$ORG/$repo.git" "$dest"
  if [ "$layout" = multi ] && [ "$name" = inetutils ]; then
    ln -sfn "$dest/inetutils-services-port" "$SDK_WORK/inetutils-services-port"
  fi
done
# the two srcroot forks that older Makefile targets reach OUTSIDE port dirs:
[ -d "$HOME/Projects/vim-nanos/.git" ] || git clone -q "$ORG/vim-nanos.git" "$HOME/Projects/vim-nanos"
ln -sfn "$HOME/Projects/vim-nanos" "$SDK_WORK/vim"
mkdir -p "$SDK_WORK/vim-port" && rsync -a "$HOME/Projects/vim-nanos/nxport.toml" "$SDK_WORK/vim-port/" && \
  rsync -a "$HOME/Projects/vim-nanos/hooks" "$SDK_WORK/vim-port/" 2>/dev/null || true
[ -d "$SDK_WORK/ncurses/.git" ] || git clone -q "$ORG/ncurses-nanos.git" "$SDK_WORK/ncurses"
mkdir -p "$SDK_WORK/ncurses-port" && rsync -a "$SDK_WORK/ncurses/nxport.toml" "$SDK_WORK/ncurses-port/" 2>/dev/null || true
[ -d "$HOME/Projects/bash-nanos/.git" ]    || git clone -q "$ORG/bash-nanos.git"    "$HOME/Projects/bash-nanos"
[ -d "$HOME/Projects/netsurf-nanos/.git" ] || git clone -q "$ORG/netsurf-nanos.git" "$HOME/Projects/netsurf-nanos"

step "toolchain (i686 + x86_64)"
if [ ! -x "$SDK_WORK/toolchain/bin/x86_64-nanos-gcc" ]; then
  "$HERE/build-toolchain.sh"
else echo "  toolchain: present"; fi

step "docker images"
docker image inspect nanos-sdk-dev:latest >/dev/null 2>&1 || docker build -t nanos-sdk-dev:latest "$HERE"
echo "  (nanos-build image is built by the NanOS repo: make docker-image)"

step "next steps (run inside the NanOS checkout)"
cat <<'EOF'
  make docker-image
  make ARCH=x86_64 zlib openssl ncurses            # library layer
  make ARCH=x86_64 toybox sudo grep bzip2          # base userland
  make ARCH=x86_64 ping wget inetd httpd udhcpc dropbear   # network layer
  make ARCH=x86_64 vim htop git sqlite bash        # apps
  make ARCH=x86_64 libpng libjpeg && make netsurf  # browser
  make libdrm mesa gles2info glkms nwm-gl          # GL userspace
  make externals && make image64                   # the image
  # host GL stack (macOS, optional): see qemu-nanos/nanos/build-qemu.sh + virglrenderer-nanos/nanos/build-virgl.sh
EOF
```

```bash
chmod +x ~/Projects/nanos-sdk/scripts/bootstrap.sh && shellcheck ~/Projects/nanos-sdk/scripts/bootstrap.sh
```

- [ ] **Step 2: Verify idempotence on the LIVE workspace (no-op run)**

```bash
cd ~/Projects/nanos-sdk && SDK_WORK=~/Projects/nanos-sdk-work scripts/bootstrap.sh
```

Expected: every clone line prints `present`, toolchain `present`, docker image found — and nothing in the live workspace is modified. (Manifest checkouts exist as plain dirs, not clones, on this machine — the script must print `present` for those too: adjust the check `[ -d "$dest/.git" ]` to `[ -e "$dest" ]`; make that change now and re-run.)

- [ ] **Step 3: Commit + push**

```bash
cd ~/Projects/nanos-sdk && git add scripts/bootstrap.sh && git commit -m "bootstrap: fresh machine -> populated sdk-work + toolchain + docker, manifest-driven" && git push
```

### Task 13: `docs/ECOSYSTEM.md` in NanOS + README link

**Files:**
- Create: `~/Projects/NanOS/docs/ECOSYSTEM.md`
- Modify: `~/Projects/NanOS/README.md` (add a short section)
- Modify: `~/Projects/NanOS/docs/superpowers/plans/README.md` (index entry + Plan-1 supersession note)

- [ ] **Step 1: Write ECOSYSTEM.md**

Content requirements (write in full, English, from the audit data in the spec and the completed tasks — the writer has all facts in the spec + `gh repo list NanOS-labs`):
1. **Repo table** — every NanOS-labs repo: upstream base + version, what we changed (one line), which NanOS make target consumes it, bootstrap checkout path (copy the `checkout` column from ports.manifest).
2. **Dependency order** — the layered build list exactly as bootstrap.sh prints it.
3. **Fresh-machine walkthrough** — `git clone NanOS-labs/nanos-sdk && scripts/bootstrap.sh`, then the make sequence, ending at `make run64` / `make flash-dell`; separate subsection for the macOS host GL stack (qemu-nanos + virglrenderer-nanos builds, startergo angle/libepoxy brew deps with the codesign re-sign quirk, `make run64-gl-desktop`).
4. **Host dependency list** — Docker, brew formulas + taps with the versions currently installed (`brew list --versions qemu e2fsprogs`, `brew list --versions startergo/...`).
5. **Not forked, by design** — gcc/binutils/picolibc (generated by nanos-sdk `patch.sh`; musl/picolibc audit verdict from Task 7), `~/Projects/git` clean clone, startergo angle/libepoxy host dylibs (documented, pinned, not ours).

- [ ] **Step 2: README section + plans index**

Append to README.md a short "## Ecosystem & reproducibility" section: two sentences (org = NanOS-labs, everything rebuildable from scratch) + link to `docs/ECOSYSTEM.md`. Update `docs/superpowers/plans/README.md`: add this plan with status, mark Plan 1's recipe-home decision superseded by the 2026-07-12 spec.

- [ ] **Step 3: Commit + push**

```bash
cd ~/Projects/NanOS
git add docs/ECOSYSTEM.md README.md docs/superpowers/plans/README.md
git commit -m "docs: ECOSYSTEM.md - the NanOS-labs repo map, dependency graph and fresh-machine walkthrough"
git push origin develop
```

### Task 14: Clean-room reproducibility gate

- [ ] **Step 1: Bootstrap into a fresh SDK_WORK**

```bash
cd ~/Projects/nanos-sdk && SDK_WORK=/tmp/fresh-sdk-work scripts/bootstrap.sh
```

Expected: all clones happen for real this time; toolchain step either builds (~40 min) or — acceptable shortcut for THIS gate — is rsync-copied from the live toolchain with a log note, since toolchain build reproducibility is already covered by nanos-sdk CI/plans. Everything below `clone` must come from the org, not the old workspace.

- [ ] **Step 2: Build the world against the fresh workspace**

```bash
cd ~/Projects/NanOS
export SDK_WORK=/tmp/fresh-sdk-work
make docker-image
make ARCH=x86_64 zlib openssl ncurses
make ARCH=x86_64 toybox sudo grep bzip2
make ARCH=x86_64 ping wget inetd httpd udhcpc dropbear
make ARCH=x86_64 vim htop git sqlite bash
make ARCH=x86_64 libpng libjpeg && make netsurf
make libdrm mesa gles2info glkms nwm-gl
make externals && make image64 && make image64-gl
```

Expected: every target succeeds sourcing ONLY /tmp/fresh-sdk-work. Failures here are the gate doing its job — each missing file is a recipe that didn't make it into a fork; fix the fork (add + push the file), re-run.

- [ ] **Step 3: Verify the OS + GL desktop**

```bash
cd ~/Projects/NanOS && SDK_WORK=/tmp/fresh-sdk-work make verify64      # full smoke suite, green
SDK_WORK=/tmp/fresh-sdk-work make run64-gl-desktop                     # boots to the GL desktop (visual check, jan/jan on F7)
```

Expected: verify64 all-green; GL desktop composites.

- [ ] **Step 4: Record the gate + clean up**

```bash
cd ~/Projects/NanOS
# append a "verified on <date>, clean-room /tmp/fresh-sdk-work" line to docs/ECOSYSTEM.md
git commit -am "docs: ecosystem clean-room gate passed (fresh SDK_WORK bootstrap -> image64 + verify64 + GL desktop)"
git push origin develop
rm -rf ~/Projects/nanos-sdk-work/_migrate /tmp/fresh-sdk-work
```

Also update project memory (`nanos-ecosystem-forks.md`): step zero DONE, follow-up = build unification.

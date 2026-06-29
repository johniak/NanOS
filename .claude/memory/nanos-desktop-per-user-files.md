---
name: nanos-desktop-per-user-files
description: desktop per-user files must live in $HOME (not root-owned /nanos/config); greeter HOME env was a latent bug
metadata: 
  node_type: memory
  type: project
  originSessionId: 661d85b4-63ba-4deb-8adc-c850a1d3125e
---

The x86_64 desktop (nwm + apps) runs as the logged-in user (jan, uid 1000) — the greeter
setuid's before exec. So per-user files must live under `$HOME` (`/disks/main/users/jan`,
which jan owns), NOT under `/disks/main/nanos/config` — that dir is root-owned 0755 (holds
passwd/shadow/sudoers) and the unprivileged desktop cannot create files there (writes fail
EACCES, silently). The `$HOME/.nanos-open` open-with store already followed this; desktop
**settings** now do too: `$HOME/.nanos-settings.yaml` (load layers shipped defaults →
system NW_SETTINGS_PATH → per-user file; save targets per-user). See [[nanos-ui-redesign]],
[[nanos-rust-file-explorer]].

**Latent bug fixed alongside (2026-06-28, branch feat/rust-explorer):** the greeter built the
login env by copying init's env (init = root, HOME=/disks/main/root) then APPENDING the user's
HOME — and `getenv()` returns the FIRST match, so the whole desktop's `getenv("HOME")` resolved
to root's home. Every $HOME-keyed lookup (settings, open-with) silently missed. Fix:
`build_env` now drops inherited HOME/USER/LOGNAME/SHELL/PATH before appending its own. The
user-visible symptom was "transparency toggle in Settings does nothing" — actually nothing in
Settings applied. Commits 2e32912 (greeter HOME) + 23f48f5 (per-user settings).

**More fixes in the same session (all on feat/rust-explorer):**
- nwm spawned child apps with a hardcoded env of just `NW_DISPLAY=1` (no HOME/USER/PATH), so
  Settings/Files/etc. couldn't resolve $HOME → read/wrote different files than nwm. Fix: build
  the child env from nwm's own login env + NW_DISPLAY (commit b92e038). Without this the per-user
  settings fix above was inert.
- `open <relative-file>` failed: the launched app runs with the COMPOSITOR's cwd (the user's
  home), not the terminal's, so a bare `open report.txt` looked in the wrong dir. Fix: `open`
  canonicalizes via getcwd() before sending (commit b756638), like macOS `open`.
- `nanosu` (setuid-root "authenticate-to-open" helper) only accepted ROOT's password, but the
  dialog says "administrator password" and the user (jan) is a wheel admin. Now it accepts the
  invoking wheel-member's OWN password (macOS/sudo style), root's still works (commit 78c1548).
  KNOWN GAP: a WRONG password in the auth dialog fails silently (nwm fire-and-forgets nanosu, no
  re-prompt / GUI feedback) — a follow-up if it bites.

---
name: no-claude-attribution-in-commits
description: "Git commits must contain no mention of Claude — no Co-Authored-By, no \"Generated with\" trailer"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 661d85b4-63ba-4deb-8adc-c850a1d3125e
---

Git commit messages (and PR bodies) must contain NO reference to Claude whatsoever — no `Co-Authored-By: Claude` trailer, no "Generated with Claude Code" line, nothing.

The user ALSO wants a commit after **every completed phase** (of a multi-phase plan), proactively — don't wait to be asked each time. One clean commit per phase as it lands.

**Why:** The user wants AI invisible in the project's git history, and wants the history to track progress phase-by-phase.

**How to apply:** Commit after each phase completes (work on the feature branch, e.g. `dockerized-build`, not master). Omit the default Co-Authored-By and "Generated with Claude Code" trailers entirely — clean, substantive messages only. This overrides the harness default that appends those trailers. Do NOT push unless asked. Never `git add` CLAUDE.md.

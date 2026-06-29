---
name: no-shortcuts-on-foundations
description: "User insists foundational/base layers (libraries, ports, core infra) be done properly — no stopgaps"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: d9f3d2b3-9551-4707-bd67-d80a786155b3
---

When building on NanOS (especially ports), the user requires that **base/foundation layers are done
properly, not with shortcuts** ("ważne żeby bazy nie były robione na skróty"). Stopgaps in a
foundation corrupt every layer above and are hard to debug later.

**Why:** a fast foundation that has to be redone is slower than a correct one; corners cut in base
libraries surface as mysterious bugs in dependents.

**How to apply:**
- Full, correct feature builds — disabling a feature is OK only for genuine platform scope, and each
  disable carries a `# why:` note. "Disabled because it failed to build" is not acceptable — fix it.
- No deferred foundational behavior (e.g. charset/iconv conversion, TLS verification, image-decode
  correctness): do it right the first time, not "subset now, revisit later." (Concrete instance: in
  the NetSurf port plan I replaced a built-in-charset stopgap with a real GNU libiconv port.)
- Verify against the upstream **test suite**, not just a link/smoke test, for base libraries.
- Derive autotools `cache`/config forcings and audit the generated config — don't guess and patch reactively.

Lives in the NetSurf port plan [[nanos-netsurf-browser-port]] but applies generally.

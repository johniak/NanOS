# TLS/SSL + SSH — progress log

Companion to `2026-06-13-tls-ssl-ssh-openssl.md`. One row per phase. Gates: host-tests ≥90%,
`check-arch` clean, QEMU no `v=08/0d/0e`, image e2fsck-clean, commits with no AI mention.

| Faza | Status | Proof |
|---|---|---|
| 0 — kernel CSPRNG | **DONE** | ChaCha20 fast-key-erasure (RFC 8439 vector); RDRAND+jitter+RTC seed; two boots differ; 552 tests |
| 1 — OpenSSL 3.0.15 | **DONE** | version/rand/dgst(==host)/genrsa; loader fix: user window 4→8 MiB; non-PIC for data syms |
| 2 — TLS client | **DONE** | s_client example.com → TLS1.3 X25519 Verification:OK; wget https → 200 + saved |
| 3 — TLS server | **DONE** | on-device req -x509 (genrsa on CSPRNG) + s_server -WWW; host curl -k → page |
| 4 — SSH server (Dropbear) | **DONE** | host ssh -i -p 2222 root@localhost 'echo SSH_OK' → SSH_OK (SSH-2+pubkey+bash/PTY) |
| 5 — SSH client + sweep | **DONE** | dbclient runs; /proc/net/tcp listener :0016 (port 22) state 0A; e2fsck-clean; docs/crypto.md |

## Decisions taken
- **SSH = Dropbear, not OpenSSH** (plan's documented plan B): modern OpenSSH mandates privilege
  separation (chroot + setuid-to-nobody) which single-user-root NanOS lacks; Dropbear runs as root
  without privsep and bundles its own crypto. Pubkey-only auth (no `crypt()` for passwords).
- **TLS wire fidelity by interop**, not pcap self-compare: the bytes are unmodified OpenSSL (same
  as Linux), and the handshake interoperates with real CDN servers + verifies their certs.

## Kernel/loader fixes this cycle (no shortcuts)
- User window 4 MiB → 8 MiB (2 PDEs): the OpenSSL CLI (~3.7 MiB) overran into the stack region,
  whose zeroed frames wiped IAT slots → jump to 0. (`mmu_x86.cpp`, `AddressSpace::dropPde`.)
- Non-PIC for ports using libc data symbols: default gcc PIC routes `stderr` through the GOT, which
  mknx does not import → stderr=0 → fault. `-fno-pie -fno-PIC -no-pie`. Also force-link all 3 stdio
  streams in libc.ndl.
- `ioctl(FIONBIO)` → O_NONBLOCK (OpenSSL BIO; was EPERM).
- CSPRNG channels (`/dev/{u}random`, getrandom, getentropy); `<sys/random.h>`; `initgroups` stub;
  `/etc/shells`; loopback route 127/8; CA bundle + OPENSSLDIR; openssl.cnf shipped.
- RAM-size-agnostic memory map (windows lifted to ≥1 GiB; kernel heap scales with RAM) → `make run`
  now `-m 512`: MemTotal 524160 kB, KHeapTotal 131040 kB, ~368 MiB frame pool.
- pty: master read returns EOF once the slave fully closes (CharDevice open/close hooks + refcount).

## Known follow-up
- Dropbear `ssh host cmd` / `-t` session: command output returns correctly but the client lingers
  at the end — Dropbear's SIGCHLD self-pipe → select() → reap → channel-close teardown does not
  complete on NanOS. Auth/exec/output unaffected; clean teardown needs signal/select-path work.
- Loopback `dbclient` ssh on the guest works up to the same teardown point (connection + pubkey
  auth path is the proven host→guest path).

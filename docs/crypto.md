# NanOS cryptography: CSPRNG, TLS, SSH

NanOS has a real transport-security stack: a kernel CSPRNG, a port of OpenSSL (libcrypto +
libssl + the `openssl` CLI), HTTPS as both client and server, and an SSH-2 server. This file
documents how the pieces fit; the build/port flow is in `CLAUDE.md` and the plan/progress under
`docs/superpowers/plans/2026-06-13-tls-ssl-ssh-openssl.md`.

## 1. The kernel CSPRNG (`kernel/Csprng.*`)

Everything that needs unpredictable bytes draws from ONE shared kernel CSPRNG — never a fixed
seed (an earlier `/dev/random` and `getentropy` used constant seeds, so every boot produced the
same stream and any key derived from them was predictable; that was the security gate the whole
stack depended on).

- **Construction:** ChaCha20 in a "fast key erasure" arrangement. Each refill runs one RFC 8439
  ChaCha20 block; the first 32 output bytes overwrite the key (forward secrecy) and the rest is
  handed out. The block function is verified against the RFC 8439 known-answer vector
  (`tests/test_csprng.cpp`); the class is machine-independent and host-tested (≥90%).
- **Seeding:** at boot `csprngKernelSeed()` mixes RDRAND (when the CPU exposes it — `make run`
  passes `-cpu Nehalem` so QEMU does), folded RDTSC timing jitter, and the RTC epoch. RDRAND/RDTSC
  live only in `arch/x86/cpu/random_x86.cpp` behind the `<arch/random.h>` contract (`check-arch`
  clean). Two separate boots produce DIFFERENT bytes — the non-determinism gate.
- **Exposure:** `/dev/random` + `/dev/urandom` (identical — the CSPRNG never blocks and is always
  seeded), `getrandom(2)` (SYS_getrandom 355), and `getentropy(3)` all draw from it. OpenSSL seeds
  its own DRBG from `/dev/urandom` (`--with-rand-seed=devrandom`). `randhex` prints both channels.

## 2. OpenSSL (`make openssl` -> /nanos/bin/openssl)

OpenSSL 3.0.15, unmodified upstream, cross-built via the nanos-sdk (`no-asm/no-threads/no-shared`,
fixed-base ET_EXEC). libcrypto.a/libssl.a + headers install into the SDK sysroot so other ports
link them. The CLI works on the guest: `openssl version`, `rand -hex` (CSPRNG, differs per call),
`dgst -sha256` (matches the host byte-for-byte), `genrsa 2048` (real key on the CSPRNG).

## 3. TLS client

- `openssl s_client -connect <host>:443` over the slirp NAT completes a real TLS 1.3 handshake to
  a live internet host (X25519, AES-256-GCM) and verifies the chain against the shipped Mozilla CA
  bundle (`/nanos/ssl/cert.pem`, OpenSSL's compiled OPENSSLDIR) — `Verification: OK`.
- `wget https://…` (rebuilt `--with-ssl=openssl`): DNS -> TCP -> TLS (cert-verified) -> HTTP 200 ->
  file saved on the guest.
- The TLS bytes are produced by unmodified OpenSSL, so wire fidelity is inherent; interop with real
  CDN servers (which would reject a malformed handshake) is the proof.

## 4. TLS server

The guest self-signs a cert on-device (`openssl req -x509 -newkey rsa:2048`, genrsa + X.509 sign on
the CSPRNG) and serves files with `openssl s_server -accept 5443 -WWW`. From the host:
`curl -k https://localhost:5443/` returns the page (full server-side TLS 1.3 to a host client).

## 5. SSH server (Dropbear)

`make dropbear` builds Dropbear 2022.83 (server `dropbear` + `dropbearkey` + client `dbclient`),
unmodified upstream (config via `localoptions.h`). Chosen over OpenSSH because it runs as root
WITHOUT privilege separation (no chroot/setuid-to-nobody, which modern OpenSSH mandates) and
bundles its own crypto.

Auth supports both **password** and **public key**:
- **Password** (no key management): NanOS's `crypt(3)` (SHA-512 `$6$`, see above) backs it; root's
  hash is in `/nanos/config/passwd`. From the host: `ssh -p 2222 root@localhost` -> password `nanos`.
- **Public key:** put the client's key in `~/.ssh/authorized_keys` (root's home is `/disks/main/root`):
  `ssh -i key -p 2222 root@localhost`.

Either way you get a bash session over the kernel PTY: SSH-2 banner, KEX, ed25519 host key, auth,
shell — all on the guest. (Start the server first: `dropbearkey -t ed25519 -f /tmp/hk` then
`dropbear -r /tmp/hk -p 22`; the host reaches it via the `make run` hostfwd 2222->22.)

**Known limitation:** a non-interactive `ssh host cmd` (and a `-t` pty session) returns the command
output correctly but the client lingers at the end instead of closing cleanly — Dropbear's
session teardown (its SIGCHLD self-pipe -> select() -> reap -> channel-close chain) does not
complete on NanOS. The auth/exec path and output are unaffected; clean channel teardown is a
follow-up in the signal/select path. (The pty itself now reports EOF on slave close — proper tty
semantics — but Dropbear closes on the self-pipe wakeup, not the master EOF.)

## Build knobs worth knowing

- A port that uses libc data symbols (`stderr`/`stdout`) MUST build **non-PIC** (`-fno-pie
  -fno-PIC -no-pie`): default gcc PIC routes data symbols through the GOT, and mknx does not turn
  GOT data references into `.nxe` imports, so the symbol resolves to address 0 and faults. (libc.ndl
  force-links all three stdio streams so they export.)
- `-cpu Nehalem` exposes RDRAND; `-m 512` (now the default for `make run`) — the memory map places
  its windows above RAM, so RAM scales freely up to ~1 GiB.

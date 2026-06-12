# strace fixtures — the syscall contract our network stack must satisfy

These are **real** `strace` captures of the exact target applications (FAZA 0 of
`docs/superpowers/plans/2026-06-12-networking.md`), taken on a stock Debian trixie Linux:

```
strace -f -s 80 -e trace=%network,openat -o <out>.strace <cmd>
```

| fixture | command | exercises |
|---|---|---|
| `inetutils-ping-numeric.strace`  | `ping -c1 1.1.1.1` | RAW/ICMP path, no DNS |
| `inetutils-ping-hostname.strace` | `ping -c1 wp.pl`   | DNS (UDP/53) + RAW/ICMP |
| `wget-http.strace`               | `wget http://example.com` | DNS + TCP + `MSG_PEEK` |

The syscall **names**, sockopt constants, struct usage and `/etc` paths are identical on i686
and amd64 — only the syscall *numbers* differ, and those we map ourselves (table below). So an
amd64 capture is a faithful contract for an i686 target. This is the input contract for FAZY
7–13 and the reference for the "semantic strace parity" gate (§2 of the plan): the same
operation must return the same values/errno and block the same way on NanOS.

## Distilled contract (what the kernel + SDK MUST provide)

### inetutils `ping` (the FAZA 13 success target)
- `socket(AF_INET, SOCK_RAW, IPPROTO_ICMP)` — a **raw ICMP** socket (NOT iputils' ICMP-datagram).
- `setsockopt(fd, SOL_SOCKET, SO_BROADCAST, 1)`.
- For a hostname: glibc resolver (see below) resolves to an A record first.
- `sendto(raw, <8B ICMP echo hdr + 56B payload = 64B>, 0, {AF_INET, port 0, dst}, 16)`.
- `recvfrom(raw, buf 136, 0, {AF_INET,...}, [16])` → returns **20B IP + 64B ICMP = 84B** (raw
  sockets deliver the IP header too — our RAW path must prepend it on RX).
- Reads `/etc/nsswitch.conf`, `/etc/protocols`.

### `wget` (the FAZA 13 second proof, TCP)
- Resolver (DNS) → A record.
- `socket(AF_INET, SOCK_STREAM, IPPROTO_IP)` → `connect({AF_INET, port 80, dst}, 16)`.
- **`recvfrom(fd, buf, len, MSG_PEEK, NULL, NULL)`** in a loop — TCP recv MUST honor `MSG_PEEK`.
- Tries `socket(AF_INET6,...)`/`connect` first → must fail cleanly (`ENETUNREACH`/`EAFNOSUPPORT`)
  so wget falls back to IPv4. (FAZA 11 "semantyka bez IPv6".)

### glibc stub resolver (what `getaddrinfo` does — we ship our OWN equivalent in the SDK libc)
- Reads `/etc/nsswitch.conf`, `/etc/resolv.conf`, `/etc/hosts` (so FAZA 10 must populate `/etc`).
- `socket(AF_INET, SOCK_DGRAM, IPPROTO_IP)`, `setsockopt(SOL_IP, IP_RECVERR, 1)`,
  `connect({AF_INET, port 53, nameserver}, 16)`, `sendto(query)`, `recvfrom(response)`.
- Query wire format (from the capture): `<id> 01 00 0001 0000 0000 0000  02 'wp' 02 'pl' 00  0001 0001`
  — standard A query, RD=1, one question, QTYPE=A(1), QCLASS=IN(1), name in length-prefixed labels.
- **Deliberately NOT replicated:** glibc also opens `AF_NETLINK/RTM_GETADDR` (RFC 3484 source-address
  sorting) and `nscd` AF_UNIX sockets. Our SDK ships a **musl-style resolver that does neither** —
  it degrades exactly as glibc does when netlink/nscd are absent (both seen failing with ENOENT in
  the capture and the app proceeds). This is why the plan ships our own resolver, not glibc nss.

## i386 (i686) syscall numbers we therefore must implement (FAZA 9)

`socketcall`=102 (demux) **and** the direct calls (Linux ≥4.3):
`socket`=359, `socketpair`=360, `bind`=361, `connect`=362, `listen`=363, `accept4`=364,
`getsockopt`=365, `setsockopt`=366, `getsockname`=367, `getpeername`=368, `sendto`=369,
`sendmsg`=370, `recvfrom`=371, `recvmsg`=372, `shutdown`=373. Plus `_newselect`=142 (FAZA 9),
`poll`=168 (exists). `send`/`recv` exist ONLY as socketcall sub-calls (SYS_SEND=9, SYS_RECV=10),
never as direct i386 syscalls — matching Linux.

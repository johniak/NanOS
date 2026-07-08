#!/usr/bin/env bash
# smoke-x86_64.sh — the machine-dependent (MD) boot smoke for x86_64, the half `make ARCH=x86_64
# test` CANNOT cover: it boots the real disk image in QEMU headless and asserts the whole MD path
# came up with ZERO faults — long-mode entry, GDT/IDT/paging, ATA + ext4 JBD2 write, e1000/net,
# the scheduler, and a ring-3 fork/exec (init -> bash) driven from the console. One reproducible
# command, so "does x86_64 still work end-to-end" is checkable in ~30s instead of by eyeballing.
#
# Usage: scripts/smoke-x86_64.sh            (boots disk/image64.img; build it first)
# Exit 0 = PASS, non-zero = FAIL (with the offending serial/int-log lines printed).
set -u
IMG=disk/image64.img
SER=/tmp/nanos-x64smoke-serial.log
INT=/tmp/nanos-x64smoke-int.log
MON=/tmp/nanos-x64smoke-qmon.sock
rm -f "$SER" "$INT" "$MON"
[ -f "$IMG" ] || { echo "FAIL: $IMG missing — run 'make image64' first"; exit 2; }

pkill -9 -f "qemu-system-x86_64.*$IMG" 2>/dev/null
qemu-system-x86_64 -cpu qemu64 -m 512 -drive file="$IMG",format=raw \
    -netdev user,id=n0,hostfwd=tcp::2222-:22 -device e1000,netdev=n0 \
    -display none -serial file:"$SER" -monitor unix:"$MON",server,nowait \
    -no-reboot -d int,cpu_reset -D "$INT" &
QPID=$!
cleanup() { kill -9 "$QPID" 2>/dev/null; }
trap cleanup EXIT

# 1) wait for the login prompt (means: long-mode + paging + ext mount + scheduler + init->login all ran)
for i in $(seq 1 40); do grep -q "nanos login:" "$SER" 2>/dev/null && break; sleep 1; done

# 2) log in as jan (login shell = bash) then drive a ring-3 fork/exec: bash runs `echo`, proving
#    keyboard -> tty -> login -> fork/exec. jan/jan come from the seeded /etc account database.
python3 - "$MON" <<'PY'
import socket,time,sys
KM={' ':'spc','_':'shift-minus','\n':'ret'}
def kn(c):
    if c in KM: return KM[c]
    if c.isdigit(): return c
    if c.isalpha(): return ('shift-'+c.lower()) if c.isupper() else c
    return None
s=socket.socket(socket.AF_UNIX)
try: s.connect(sys.argv[1])
except Exception as e: print("monitor connect failed:",e); sys.exit(0)
time.sleep(0.3)
try: s.settimeout(0.3); s.recv(65536)
except: pass
def typ(text, settle):
    for c in text:
        k=kn(c)
        if k: s.sendall(("sendkey "+k+"\n").encode()); time.sleep(0.04)
        try: s.settimeout(0.1); s.recv(4096)
        except: pass
    s.sendall(b"sendkey ret\n"); time.sleep(settle)
typ("jan", 2.5)                    # username (generous settles: boot now brings up 6 getty logins
typ("jan", 4.0)                    # + nwm, so login/bash can lag under verify64's concurrent load)
typ("echo X64_SMOKE_FORK_OK", 1.0) # a command at the shell -> fork/exec
typ("malloctest", 6.0)             # libc allocator gates: 16-byte alignment + 72 MiB heap ceiling
                                   # (72 MiB of page-touching needs a longer settle under TCG)
s.close()
PY
sleep 3

# ---- assertions -------------------------------------------------------------------------------
PASS=1
chk() { if grep -q "$1" "$SER" 2>/dev/null; then echo "  OK  : $2"; else echo "  FAIL: $2 (missing: $1)"; PASS=0; fi; }
no()  { if grep -qE "$1" "$SER" 2>/dev/null; then echo "  FAIL: $2"; grep -E "$1" "$SER" | head -3 | sed 's/^/        /'; PASS=0; else echo "  OK  : $2"; fi; }

echo "=== x86_64 MD boot smoke ==="
chk "jan@nanos"                               "logged in -> bash login shell (boot+paging+ext+sched+init->login->bash)"
chk "EXT-RW selftest: write+read OK"          "ext4 JBD2 write path (selftest)"
chk "eth0 .* up\|Networking: lo + eth0"        "networking (e1000 + net threads) up"
chk "X64_SMOKE_FORK_OK"                        "ring-3 fork/exec from console (echo ran, output returned)"
chk "malloctest: MALLOC_ALIGN16 PASS"          "libc malloc/calloc/realloc are 16-byte aligned (ABI max_align_t)"
no  "MALLOC_ALIGN16 FAIL"                       "no under-aligned allocation from the C allocator"
chk "malloctest: HEAP_BIG PASS"                "brk heap grows past the old 64 MiB cap (GL desktop needs it)"
no  "HEAP_BIG FAIL"                             "no premature heap-ceiling malloc failure"
no  "CPU EXCEPTION|KERNEL EXCEPTION|killed faulting process|Triple fault"  "no faults on the console"
# QEMU int log: a guest reset / triple fault would show here even if the console didn't
if grep -qE "Triple fault" "$INT" 2>/dev/null; then echo "  FAIL: QEMU logged a Triple fault"; PASS=0; else echo "  OK  : no triple fault in QEMU int log"; fi

echo "============================="
if [ "$PASS" = 1 ]; then echo "x86_64 MD smoke: PASS"; exit 0; else echo "x86_64 MD smoke: FAIL"; exit 1; fi

#!/usr/bin/env bash
# httpd-qemu.sh — FAZA H4 acceptance: boot NanOS, start darkhttpd serving /apps/www on :80, then
# fetch it from the macOS host (localhost:5555 -> guest:80 via slirp hostfwd). Checks a single GET
# returns 200 + the page, then fires 4 concurrent GETs (TCB_N=16 gives headroom).
# Usage: scripts/httpd-qemu.sh [out.png]
set -u
OUT="${1:-/tmp/nanos-httpd.png}"
IMG=disk/image-grub2.img
MON=/tmp/nanos-httpd-qmon.sock
LOG=/tmp/nanos-httpd-int.log
PPM=/tmp/nanos-httpd-screen.ppm
PCAP=/tmp/nanos-httpd.pcap
rm -f "$MON" "$LOG" "$PPM" "$PCAP"

qemu-system-i386 -drive file="$IMG",format=raw \
    -netdev user,id=n0,hostfwd=tcp::5555-:80 -device e1000,netdev=n0 \
    -object filter-dump,id=d0,netdev=n0,file="$PCAP" \
    -display none -monitor unix:"$MON",server,nowait \
    -no-reboot -d int -D "$LOG" &
QPID=$!
sleep "${BOOT_WAIT:-14}"

python3 - "$MON" "$PPM" <<'PY'
import socket, sys, time, threading
mon, ppm = sys.argv[1], sys.argv[2]
m = socket.socket(socket.AF_UNIX); m.connect(mon); time.sleep(0.3); m.recv(65536)
KEYMAP = {' ':'spc','-':'minus','/':'slash','.':'dot','&':'shift-7',':':'shift-semicolon'}
def key(k):
    m.sendall(b"sendkey "+k.encode()+b"\n"); time.sleep(0.05)
    try: m.recv(65536)
    except: pass
def typeline(cmd):
    for ch in cmd:
        if ch.isalnum(): key(ch)
        elif ch in KEYMAP: key(KEYMAP[ch])
        else: key('spc')
        time.sleep(0.10)
    key('ret'); time.sleep(0.5)

# darkhttpd serves /apps/www on :80, backgrounded so the prompt returns.
typeline("darkhttpd /disks/main/apps/www --port 80 &")
time.sleep(2.5)

def http_get(path="/", timeout=6):
    try:
        c = socket.create_connection(("127.0.0.1", 5555), timeout=timeout)
        c.settimeout(timeout)
        c.sendall(("GET %s HTTP/1.0\r\nHost: localhost\r\n\r\n" % path).encode())
        data = b""
        while True:
            try:
                d = c.recv(4096)
                if not d: break
                data += d
            except socket.timeout:
                break
        c.close()
        return data
    except Exception as e:
        return ("ERR:%s" % e).encode()

resp = http_get("/")
print("=== HTTP RESPONSE (first 400 bytes) ===")
print(resp[:400].decode("latin1", "replace"))
head = resp.split(b"\r\n", 1)[0].decode("latin1", "replace")
print("=== CHECKS ===")
print("status line :", head)
print("200 OK      :", b"200" in resp.split(b"\r\n",1)[0])
print("page body   :", b"NanOS" in resp)

# Concurrency: 4 simultaneous GETs (server-side accept loop under load; TCB_N=16).
results = [None]*4
def worker(i): results[i] = http_get("/")
ts = [threading.Thread(target=worker, args=(i,)) for i in range(4)]
for t in ts: t.start()
for t in ts: t.join()
for i, r in enumerate(results):
    line = (r or b"").split(b"\r\n",1)[0].decode("latin1","replace")
    print("  conc[%d]: %s (body=%s)" % (i, line, bool(r and b"NanOS" in r)))
ok = sum(1 for r in results if r and b"200" in r.split(b"\r\n",1)[0] and b"NanOS" in r)
print("concurrent  : %d/4 GETs returned 200 + body" % ok)

time.sleep(0.5)
m.sendall(b"screendump %s\n" % ppm.encode()); time.sleep(0.8); m.recv(65536)
m.sendall(b"quit\n"); time.sleep(0.2); m.close()
PY

wait "$QPID" 2>/dev/null
[ -f "$PPM" ] && sips -s format png "$PPM" --out "$OUT" >/dev/null 2>&1 && echo "screendump: $OUT"
echo "=== faults ==="; for v in v=08 v=0d v=0e; do echo "  $v : $(grep -c "$v" "$LOG" 2>/dev/null)"; done
echo "=== pcap (:80) ==="; tcpdump -nr "$PCAP" 'tcp port 80' 2>/dev/null | head -8

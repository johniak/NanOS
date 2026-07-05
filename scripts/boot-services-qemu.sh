#!/usr/bin/env bash
# boot-services-qemu.sh — FAZA H5 acceptance: boot NanOS and, WITHOUT typing anything on the
# console, reach every listening service from the macOS host through slirp hostfwd. init starts
# inetd + darkhttpd automatically after DHCP, so the system is "visitable" straight off a boot:
#   - HTTP   : curl-equivalent GET http://localhost:5555/  (darkhttpd on :80)
#   - daytime: connect localhost:5013                      (inetd built-in on :13)
#   - echo   : connect localhost:5007 + send a line        (inetd built-in on :7)
#   - telnet : login on localhost:2323                     (telnetd on :23 -> bash)
# Usage: scripts/boot-services-qemu.sh [out.png]
set -u
OUT="${1:-/tmp/nanos-bootsvc.png}"
IMG=disk/image.img
MON=/tmp/nanos-bsvc-qmon.sock
LOG=/tmp/nanos-bsvc-int.log
PPM=/tmp/nanos-bsvc-screen.ppm
PCAP=/tmp/nanos-bsvc.pcap
rm -f "$MON" "$LOG" "$PPM" "$PCAP"

qemu-system-i386 -drive file="$IMG",format=raw \
    -netdev user,id=n0,hostfwd=tcp::5555-:80,hostfwd=tcp::2323-:23,hostfwd=tcp::5007-:7,hostfwd=tcp::5013-:13 \
    -device e1000,netdev=n0 \
    -object filter-dump,id=d0,netdev=n0,file="$PCAP" \
    -display none -monitor unix:"$MON",server,nowait \
    -no-reboot -d int -D "$LOG" &
QPID=$!
sleep "${BOOT_WAIT:-16}"   # boot + DHCP lease + init starts the services + shell prompt

python3 - "$MON" "$PPM" <<'PY'
import socket, sys, time
mon, ppm = sys.argv[1], sys.argv[2]
m = socket.socket(socket.AF_UNIX); m.connect(mon); time.sleep(0.3); m.recv(65536)

def grab(port, send=b"", n=256, timeout=5):
    try:
        c = socket.create_connection(("127.0.0.1", port), timeout=timeout); c.settimeout(timeout)
        if send: c.sendall(send)
        data=b""
        try:
            while len(data)<n:
                d=c.recv(n-len(data))
                if not d: break
                data+=d
        except socket.timeout: pass
        c.close(); return data
    except Exception as e:
        return ("ERR:%s"%e).encode()

http = grab(5555, b"GET / HTTP/1.0\r\nHost: x\r\n\r\n", 512)
day  = grab(5013, b"", 64)
ech  = grab(5007, b"hi-there\n", 9)

# telnet: refuse all options, then read the login banner/prompt
IAC,DONT,DO,WONT,WILL,SB,SE=255,254,253,252,251,250,240
def telneg(data, sock):
    out=bytearray(); resp=bytearray(); i=0
    while i<len(data):
        b=data[i]
        if b==IAC and i+1<len(data):
            cmd=data[i+1]
            if cmd in (DO,DONT,WILL,WONT) and i+2<len(data):
                opt=data[i+2]
                if cmd==DO: resp+=bytes([IAC,WONT,opt])
                elif cmd==WILL: resp+=bytes([IAC,DONT,opt])
                i+=3; continue
            elif cmd==SB:
                j=i+2
                while j+1<len(data) and not(data[j]==IAC and data[j+1]==SE): j+=1
                i=j+2; continue
            else: i+=2; continue
        out.append(b); i+=1
    if resp: sock.sendall(bytes(resp))
    return bytes(out)
def telnet_login():
    try:
        c=socket.create_connection(("127.0.0.1",2323),timeout=8); c.settimeout(0.5)
        t=bytearray(); end=time.time()+5
        while time.time()<end:
            try:
                d=c.recv(4096)
                if not d: break
                t.extend(telneg(d,c))
            except socket.timeout: pass
        c.close(); return bytes(t).decode("latin1","replace")
    except Exception as e:
        return "ERR:%s"%e

tel = telnet_login()

print("=== H5 boot-time services (no console input) ===")
print("HTTP  :80  ->", http.split(b"\r\n",1)[0].decode("latin1","replace"),
      "| body NanOS:", b"NanOS" in http)
print("daytime:13 ->", repr(day))
print("echo   :7  ->", repr(ech))
print("telnet :23 -> prompt:", any(p in tel for p in ("bash","nsh","#","$")))
print("=== CHECKS ===")
print("http   :", b"200" in http.split(b"\r\n",1)[0] and b"NanOS" in http)
print("daytime:", day.count(b":")>=2)            # "HH:MM:SS" style date
print("echo   :", b"hi-there" in ech)
print("telnet :", any(p in tel for p in ("bash","nsh","#","$")))

time.sleep(0.4)
m.sendall(b"screendump %s\n" % ppm.encode()); time.sleep(0.8); m.recv(65536)
m.sendall(b"quit\n"); time.sleep(0.2); m.close()
PY

wait "$QPID" 2>/dev/null
[ -f "$PPM" ] && sips -s format png "$PPM" --out "$OUT" >/dev/null 2>&1 && echo "screendump: $OUT"
echo "=== faults ==="; for v in v=08 v=0d v=0e; do echo "  $v : $(grep -c "$v" "$LOG" 2>/dev/null)"; done

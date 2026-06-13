#!/usr/bin/env bash
# telnet-qemu.sh — FAZA H3 acceptance: boot NanOS, start inetd, then telnet IN from the macOS host
# (localhost:2323 -> guest:23 via slirp hostfwd) and drive a real login shell. A tiny built-in
# telnet client handles the IAC option negotiation (refuses every DO/WILL), then sends shell
# commands and checks the replies. Usage: scripts/telnet-qemu.sh [out.png]
set -u
OUT="${1:-/tmp/nanos-telnet.png}"
IMG=disk/image-grub2.img
MON=/tmp/nanos-tel-qmon.sock
LOG=/tmp/nanos-tel-int.log
PPM=/tmp/nanos-tel-screen.ppm
PCAP=/tmp/nanos-tel.pcap
rm -f "$MON" "$LOG" "$PPM" "$PCAP"

qemu-system-i386 -drive file="$IMG",format=raw \
    -netdev user,id=n0,hostfwd=tcp::2323-:23 -device e1000,netdev=n0 \
    -object filter-dump,id=d0,netdev=n0,file="$PCAP" \
    -display none -monitor unix:"$MON",server,nowait \
    -no-reboot -d int -D "$LOG" &
QPID=$!
sleep "${BOOT_WAIT:-14}"

python3 - "$MON" "$PPM" <<'PY'
import socket, sys, time
mon, ppm = sys.argv[1], sys.argv[2]
m = socket.socket(socket.AF_UNIX); m.connect(mon); time.sleep(0.3); m.recv(65536)
KEYMAP = {' ':'spc','-':'minus','/':'slash','.':'dot',',':'comma','\n':'ret',
          '_':'shift-minus','=':'equal',':':'shift-semicolon','&':'shift-7'}
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

# Start inetd on the guest (daemonized). telnet -> telnetd -> nanologin -> shell.
typeline("inetd --pidfile=/tmp/inetd.pid /disks/main/nanos/config/etc/inetd.conf")
time.sleep(2.5)

# ---- minimal telnet client (host side) ----
IAC=255; DONT=254; DO=253; WONT=252; WILL=251; SB=250; SE=240
def negotiate(data, sock):
    out=bytearray(); resp=bytearray(); i=0
    while i < len(data):
        b=data[i]
        if b==IAC and i+1 < len(data):
            cmd=data[i+1]
            if cmd in (DO,DONT,WILL,WONT) and i+2 < len(data):
                opt=data[i+2]
                if cmd==DO:   resp += bytes([IAC,WONT,opt])   # refuse to do anything...
                elif cmd==WILL: resp += bytes([IAC,DONT,opt]) # ...and ask peer not to either
                i+=3; continue
            elif cmd==SB:
                # skip to IAC SE
                j=i+2
                while j+1 < len(data) and not (data[j]==IAC and data[j+1]==SE): j+=1
                i=j+2; continue
            else:
                i+=2; continue
        out.append(b); i+=1
    if resp: sock.sendall(bytes(resp))
    return bytes(out)

def drain(sock, transcript, secs):
    sock.settimeout(0.4); end=time.time()+secs
    while time.time() < end:
        try:
            d=sock.recv(4096)
            if not d: break
            transcript.extend(negotiate(d, sock))
        except socket.timeout:
            pass
    return bytes(transcript)

def session(label):
    t=bytearray()
    c=socket.create_connection(("127.0.0.1",2323), timeout=8)
    drain(c,t,3.0)                                  # login + shell prompt + IAC negotiation
    c.sendall(b"echo TELNET_MARKER_OK\n"); time.sleep(0.8); drain(c,t,1.5)
    c.sendall(b"ls /\n");                  time.sleep(0.8); drain(c,t,1.5)
    c.sendall(b"exit\n");                  time.sleep(0.8); drain(c,t,1.5)
    try: c.close()
    except: pass
    txt=bytes(t).decode("latin1","replace")
    print("=== TELNET TRANSCRIPT (%s) ===" % label)
    print(txt[-900:])
    print("--- checks (%s) ---" % label)
    print("prompt seen :", any(p in txt for p in ("bash","nsh","$","#")))
    print("echo marker :", "TELNET_MARKER_OK" in txt)
    print("ls / output :", ("disks" in txt or "tmp" in txt or "dev" in txt))
    return txt

# Two sequential sessions over the SINGLE kernel pty pair: the second proves the pty + sockets
# are released cleanly on exit (no leak — FAZA H acceptance).
try:
    session("session 1")
    time.sleep(1.5)
    session("session 2 (pty reuse)")
except Exception as e:
    print("TELNET ERROR:", e)

time.sleep(0.5)
m.sendall(b"screendump %s\n" % ppm.encode()); time.sleep(0.8); m.recv(65536)
m.sendall(b"quit\n"); time.sleep(0.2); m.close()
PY

wait "$QPID" 2>/dev/null
[ -f "$PPM" ] && sips -s format png "$PPM" --out "$OUT" >/dev/null 2>&1 && echo "screendump: $OUT"
echo "=== faults ==="; for v in v=08 v=0d v=0e; do echo "  $v : $(grep -c "$v" "$LOG" 2>/dev/null)"; done
echo "=== pcap (telnet :23) ==="; tcpdump -nr "$PCAP" 'tcp port 23' 2>/dev/null | head -10

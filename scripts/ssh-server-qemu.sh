#!/usr/bin/env bash
# FAZA 4: SSH server on the guest (Dropbear). Generate a host-side keypair, inject its PUBLIC key
# into the image at /root/.ssh/authorized_keys (offline, via debugfs), boot NanOS with hostfwd
# 2222->22, start dropbear on the guest, then from the HOST run `ssh -i key` -> a command on the
# guest's bash. Proves pubkey-auth SSH-2 remote login from the host into the guest shell.
set -u
OUTPNG="${1:-/tmp/ssh-server.png}"
IMG=disk/image-grub2.img
MON=/tmp/ssh-srv.sock; LOG=/tmp/ssh-srv.log; PPM=/tmp/ssh-srv.ppm
KEY=/tmp/nktest
rm -f "$MON" "$LOG" "$PPM" "$OUTPNG" "$KEY" "$KEY.pub"

# 1) Throwaway host keypair (NOT committed); inject the pubkey into the image's authorized_keys.
ssh-keygen -t ed25519 -f "$KEY" -N "" -q -C nanos-test
PUB=$(cat "$KEY.pub")
printf 'rm /root/.ssh/authorized_keys\n' | docker run --rm -i -v "$PWD":/s -w /s nanos-build debugfs -w "$IMG?offset=1048576" >/dev/null 2>&1
# Write the pubkey: stage it to a temp file, then debugfs-write it in.
echo "$PUB" > /tmp/ak.tmp
docker run --rm -v "$PWD":/s -v /tmp:/t -w /s nanos-build debugfs -w "$IMG?offset=1048576" \
    -R "write /t/ak.tmp /root/.ssh/authorized_keys" >/dev/null 2>&1
echo "injected authorized_keys: $PUB"

# 2) Boot with hostfwd 2222->22 (snapshot: don't persist further writes).
qemu-system-i386 -cpu Nehalem -m 512 -snapshot -drive file="$IMG",format=raw \
    -display none -monitor unix:"$MON",server,nowait -no-reboot -d int -D "$LOG" \
    -netdev user,id=n0,hostfwd=tcp::2222-:22 -device e1000,netdev=n0 &
QPID=$!
sleep "${BOOT_WAIT:-18}"

# 3) Guest: generate a host key (ed25519 is fast) and start dropbear in the foreground+background.
python3 - "$MON" <<'PY'
import socket, sys, time
mon = sys.argv[1]
KEYMAP = {' ':'spc','-':'minus','/':'slash','.':'dot',',':'comma','\n':'ret','_':'shift-minus',
          ':':'shift-semicolon','=':'equal'}
s = socket.socket(socket.AF_UNIX); s.connect(mon); time.sleep(0.3); s.recv(65536)
def send(c):
    s.sendall(c.encode()+b"\n"); time.sleep(0.04)
    try: s.recv(65536)
    except: pass
def typ(line, settle):
    for ch in line:
        if ch.isdigit() or (ch.isalpha() and ch.islower()): send("sendkey "+ch)
        elif ch.isalpha(): send("sendkey shift-"+ch.lower())
        elif ch in KEYMAP: send("sendkey "+KEYMAP[ch])
        else: send("sendkey spc")
        time.sleep(0.08)
    send("sendkey ret"); time.sleep(settle)
typ("dropbearkey -t ed25519 -f /tmp/hk", 4.0)
typ("dropbear -r /tmp/hk -p 22 -E -F &", 3.0)
s.close()
PY

# 4) HOST: ssh in with the private key and run a command. Bounded: run in the background and kill
# after 15s so a stalled session can't hang the harness; output goes to a file we then print.
echo "=== HOST ssh -i $KEY -p 2222 root@localhost 'echo SSH_OK; id; uname; ls /' ==="
( ssh -i "$KEY" -p 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
      -o ConnectTimeout=10 -o BatchMode=yes -o ServerAliveInterval=2 -o ServerAliveCountMax=3 \
      root@localhost 'echo SSH_OK; id; uname; ls / | head' > /tmp/ssh-out.txt 2>&1; echo "exit=$?" >> /tmp/ssh-out.txt ) &
SSHPID=$!
for i in $(seq 1 15); do kill -0 $SSHPID 2>/dev/null || break; sleep 1; done
kill -9 $SSHPID 2>/dev/null
cat /tmp/ssh-out.txt 2>/dev/null; echo "(ssh done)"

python3 - "$MON" "$PPM" <<'PY'
import socket, sys, time
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); time.sleep(0.3); s.recv(65536)
s.sendall(b"screendump %s\n" % sys.argv[2].encode()); time.sleep(0.8); s.recv(65536)
s.sendall(b"quit\n"); time.sleep(0.2); s.close()
PY
wait "$QPID" 2>/dev/null
[ -f "$PPM" ] && sips -s format png "$PPM" --out "$OUTPNG" >/dev/null 2>&1 && echo "screendump: $OUTPNG"
for v in "v=08" "v=0d" "v=0e"; do c=$(grep -c "$v" "$LOG" 2>/dev/null); echo "  $v : ${c:-0}"; done

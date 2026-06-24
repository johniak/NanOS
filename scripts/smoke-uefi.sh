#!/usr/bin/env bash
# smoke-uefi.sh — UEFI boot smoke: boot the GPT image with edk2/OVMF firmware (Homebrew qemu) so the
# firmware runs EFI/BOOT/BOOTX64.EFI (Limine) -> kernel -> shell. Asserts the shell is reached and the
# Multiboot1+GOP-framebuffer handoff produced zero faults. Pairs with smoke-x86_64 (BIOS/SeaBIOS).
set -u
IMG=disk/image64-grub2.img
SER=/tmp/nanos-uefi-serial.log
INT=/tmp/nanos-uefi-int.log
VARS=/tmp/nanos-uefi-vars.fd
MON=/tmp/nanos-uefi-qmon.sock
rm -f "$SER" "$INT" "$VARS" "$MON"
[ -f "$IMG" ] || { echo "FAIL: $IMG missing — run 'make image64' first"; exit 2; }

# Locate edk2/OVMF firmware (ships with Homebrew qemu).
QDIR="$(dirname "$(command -v qemu-system-x86_64)")/../share/qemu"
CODE=""
for c in "$QDIR/edk2-x86_64-code.fd" "$QDIR/edk2-x86_64-secure-code.fd" /usr/share/OVMF/OVMF_CODE.fd /opt/homebrew/share/qemu/edk2-x86_64-code.fd; do
    [ -f "$c" ] && CODE="$c" && break
done
[ -n "$CODE" ] || { echo "FAIL: edk2/OVMF firmware not found (looked in $QDIR)"; exit 2; }
# Writable vars: prefer the shipped template, else a blank 64 MiB pflash.
VARSRC=""; for v in "$QDIR/edk2-i386-vars.fd" "$QDIR/edk2-x86_64-vars.fd" /usr/share/OVMF/OVMF_VARS.fd /opt/homebrew/share/qemu/edk2-i386-vars.fd; do
    [ -f "$v" ] && VARSRC="$v" && break
done
if [ -n "$VARSRC" ]; then cp "$VARSRC" "$VARS"; else dd if=/dev/zero of="$VARS" bs=1m count=64 2>/dev/null; fi

pkill -9 -f "qemu-system-x86_64.*$IMG" 2>/dev/null
qemu-system-x86_64 -cpu qemu64 -m 512 \
    -drive if=pflash,format=raw,readonly=on,file="$CODE" \
    -drive if=pflash,format=raw,file="$VARS" \
    -drive file="$IMG",format=raw \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -display none -serial file:"$SER" -monitor unix:"$MON",server,nowait \
    -no-reboot -d int,cpu_reset -D "$INT" &
QPID=$!
trap 'kill -9 "$QPID" 2>/dev/null' EXIT
# Wait for the login prompt. OVMF + the dropbear ed25519 keygen make first boot slower than BIOS,
# so allow up to 60s. Login is mandatory (toybox `login`), so we must drive credentials — the bash
# banner never appears on its own.
for i in $(seq 1 60); do grep -q "nanos login:" "$SER" 2>/dev/null && break; sleep 1; done

# Log in as jan (login shell = bash), exactly like smoke-x86_64 — proves keyboard -> tty -> login ->
# fork/exec into bash works under the UEFI boot path too. jan/jan come from the seeded account db.
python3 - "$MON" <<'PY'
import socket,time,sys
def kn(c):
    if c.isdigit() or c.isalpha(): return c
    return {' ':'spc','_':'shift-minus','\n':'ret'}.get(c)
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
typ("jan", 1.5)                       # username
typ("jan", 2.5)                       # password -> login completes into bash
s.close()
PY
sleep 3

PASS=1
chk(){ if grep -q "$1" "$SER" 2>/dev/null; then echo "  OK  : $2"; else echo "  FAIL: $2 (missing: $1)"; PASS=0; fi; }
no(){ if grep -qE "$1" "$SER" 2>/dev/null; then echo "  FAIL: $2"; grep -E "$1" "$SER"|head -3|sed 's/^/        /'; PASS=0; else echo "  OK  : $2"; fi; }
echo "=== UEFI (OVMF) boot smoke ==="
chk "Mounting ext filesystem at /disks/main"  "ext root mounted (GPT discovery worked under UEFI)"
chk "jan@nanos"                               "reached the login shell via BOOTX64.EFI -> Limine -> kernel"
no  "CPU EXCEPTION|KERNEL EXCEPTION|killed faulting process|Triple fault"  "no faults on the console"
if grep -qE "Triple fault" "$INT" 2>/dev/null; then echo "  FAIL: QEMU logged a Triple fault"; PASS=0; else echo "  OK  : no triple fault in QEMU int log"; fi
echo "================================"
if [ "$PASS" = 1 ]; then echo "UEFI boot smoke: PASS"; exit 0; else echo "UEFI boot smoke: FAIL"; exit 1; fi

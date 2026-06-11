#!/usr/bin/env python3
# Headless QEMU driver with a FULL US-keyboard keymap so we can type shell metacharacters
# ($ | ( ) * > & etc.) into bash. Boots disk/image-grub2.img, replays steps, screendumps.
import socket, sys, time, subprocess, os

IMG="disk/image-grub2.img"; MON="/tmp/nanos-qmon.sock"; LOG="/tmp/nanos-int.log"; PPM="/tmp/nanos-screen.ppm"
OUT=sys.argv[1]; BOOT=float(sys.argv[2]); STEPS=sys.argv[3:]
for f in (MON,LOG,PPM):
    try: os.remove(f)
    except OSError: pass

q=subprocess.Popen(["qemu-system-i386","-drive",f"file={IMG},format=raw","-display","none",
    "-monitor",f"unix:{MON},server,nowait","-no-reboot","-d","int","-D",LOG])
time.sleep(BOOT)

SH={'-':'minus','=':'equal','/':'slash','.':'dot',',':'comma',';':'semicolon',
    "'":'apostrophe','\\':'backslash','[':'bracket_left',']':'bracket_right','`':'grave_accent',
    ' ':'spc','\n':'ret'}
SHIFT={'_':'minus','+':'equal','?':'slash','>':'dot','<':'comma',':':'semicolon',
    '"':'apostrophe','|':'backslash','{':'bracket_left','}':'bracket_right','~':'grave_accent',
    '!':'1','@':'2','#':'3','$':'4','%':'5','^':'6','&':'7','*':'8','(':'9',')':'0'}

s=socket.socket(socket.AF_UNIX); s.connect(MON); time.sleep(0.3)
try: s.recv(65536)
except: pass
def send(line):
    s.sendall(line.encode()+b"\n"); time.sleep(0.05)
    try: s.recv(65536)
    except: pass
def key(ch):
    if ch.isupper():  send("sendkey shift-"+ch.lower())
    elif ch.isalnum():send("sendkey "+ch)
    elif ch in SH:    send("sendkey "+SH[ch])
    elif ch in SHIFT: send("sendkey shift-"+SHIFT[ch])
    else: send("sendkey spc")
def typ(line):
    for ch in line: key(ch); time.sleep(0.08)
    send("sendkey ret"); time.sleep(0.9)

for st in STEPS:
    if st.startswith("key:"): send("sendkey "+st[4:]); time.sleep(0.7)
    elif st.startswith("wait:"): time.sleep(float(st[5:]))
    elif st.startswith("mmove:"):
        dx,dy = st[6:].split(","); send("mouse_move %s %s" % (dx,dy)); time.sleep(0.3)
    elif st.startswith("mbtn:"):
        send("mouse_button %s" % st[5:]); time.sleep(0.3)
    elif st == "dclick":                              # two fast clicks (< the double-click window)
        send("mouse_button 1"); send("mouse_button 0")
        send("mouse_button 1"); send("mouse_button 0"); time.sleep(0.3)
    elif st.startswith("raw:"):
        for ch in st[4:]: key(ch); time.sleep(0.06)   # type WITHOUT trailing Enter
    else: typ(st)

time.sleep(0.3)
s.sendall(b"screendump %s\n"%PPM.encode()); time.sleep(0.8)
try: s.recv(65536)
except: pass
s.sendall(b"quit\n"); time.sleep(0.2); s.close()
q.wait()
if os.path.exists(PPM):
    subprocess.run(["sips","-s","format","png",PPM,"--out",OUT],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    print("screendump:",OUT)
for v in ("v=08","v=0d","v=0e"):
    n=subprocess.run(["grep","-c",v,LOG],capture_output=True,text=True).stdout.strip()
    print(f"  {v} : {n}")

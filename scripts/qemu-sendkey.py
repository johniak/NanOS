#!/usr/bin/env python3
"""qemu-sendkey.py — drive a QEMU HMP monitor socket to type text or press keys.

Fable's T3 unlock: the GL desktop's typing-shred bug needs CHANGING window content to
reproduce, and scripted mouse clicks miss (PS/2 accel). The QEMU monitor `sendkey` command
delivers keystrokes straight to the focused guest window — no mouse. This maps an ASCII
string to QKeyCode `sendkey` commands (with shift- for shifted glyphs) and writes them to a
unix monitor socket.

Usage:
  qemu-sendkey.py <monitor.sock> --type 'while true; do ls; done'   # type a string
  qemu-sendkey.py <monitor.sock> --type foo --enter                 # then press Return
  qemu-sendkey.py <monitor.sock> --key ret                          # a single named key
  qemu-sendkey.py <monitor.sock> --key f7                           # e.g. VT switch
  qemu-sendkey.py <monitor.sock> --hold-delay 0.03 --type '...'     # per-key pacing
"""
import argparse, socket, sys, time

# unshifted ASCII -> QKeyCode name
UNSHIFTED = {
    'a':'a','b':'b','c':'c','d':'d','e':'e','f':'f','g':'g','h':'h','i':'i','j':'j','k':'k',
    'l':'l','m':'m','n':'n','o':'o','p':'p','q':'q','r':'r','s':'s','t':'t','u':'u','v':'v',
    'w':'w','x':'x','y':'y','z':'z',
    '0':'0','1':'1','2':'2','3':'3','4':'4','5':'5','6':'6','7':'7','8':'8','9':'9',
    ' ':'spc','-':'minus','=':'equal','[':'bracket_left',']':'bracket_right',
    '\\':'backslash',';':'semicolon',"'":'apostrophe','`':'grave_accent',
    ',':'comma','.':'dot','/':'slash','\t':'tab','\n':'ret',
}
# shifted ASCII -> QKeyCode name (sent as shift-<name>)
SHIFTED = {
    '!':'1','@':'2','#':'3','$':'4','%':'5','^':'6','&':'7','*':'8','(':'9',')':'0',
    '_':'minus','+':'equal','{':'bracket_left','}':'bracket_right','|':'backslash',
    ':':'semicolon','"':'apostrophe','~':'grave_accent','<':'comma','>':'dot','?':'slash',
}

def keys_for_char(ch):
    if ch in UNSHIFTED:
        return UNSHIFTED[ch]
    if ch.isupper() and ch.lower() in UNSHIFTED:
        return 'shift-' + UNSHIFTED[ch.lower()]
    if ch in SHIFTED:
        return 'shift-' + SHIFTED[ch]
    raise ValueError("no qcode for %r" % ch)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('sock')
    ap.add_argument('--type', default=None, help='ASCII string to type')
    ap.add_argument('--key', action='append', default=[], help='named QKeyCode(s), e.g. ret f7 ctrl-alt-f7')
    ap.add_argument('--enter', action='store_true', help='press Return after --type')
    ap.add_argument('--hold-delay', type=float, default=0.02, help='seconds between keystrokes')
    args = ap.parse_args()

    cmds = []
    if args.type is not None:
        for ch in args.type:
            cmds.append('sendkey ' + keys_for_char(ch))
    if args.enter:
        cmds.append('sendkey ret')
    for k in args.key:
        cmds.append('sendkey ' + k)

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(args.sock)
    time.sleep(0.2)
    try:
        s.recv(4096)  # drain the HMP banner
    except Exception:
        pass
    for c in cmds:
        s.sendall((c + '\n').encode())
        time.sleep(args.hold_delay)
        try:
            s.recv(4096)
        except Exception:
            pass
    s.close()

if __name__ == '__main__':
    main()

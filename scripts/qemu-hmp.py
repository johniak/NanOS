#!/usr/bin/env python3
"""qemu-hmp.py — send one HMP command to a QEMU unix monitor socket and print the reply.
Usage: qemu-hmp.py <monitor.sock> '<command>'   e.g.  qemu-hmp.py /tmp/nmon.sock 'info status'
"""
import socket, sys, time
sock, cmd = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sock)
time.sleep(0.2)
try: s.recv(65536)
except Exception: pass
s.sendall((cmd + '\n').encode())
time.sleep(0.6)
buf = b''
s.setblocking(False)
try:
    while True:
        d = s.recv(65536)
        if not d: break
        buf += d
except Exception:
    pass
s.close()
sys.stdout.write(buf.decode('utf-8', 'replace'))

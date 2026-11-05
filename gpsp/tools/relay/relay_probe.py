"""relay_probe.py -- is a GBR1 relay reachable?  Sends one PROBE to the main port and
one to the probe port (port + 1) and prints what came back: the server's view of
this machine's public address.  Harmless: a PROBE allocates nothing on the server.

    python relay_probe.py [relay.rayquaza.ca] [27420]

Run it from OUTSIDE the relay's own network (a laptop on a phone hotspot) for the
real test: many home routers do not loop a port forward back to the LAN.
"""
import os
import socket
import struct
import sys

host = sys.argv[1] if len(sys.argv) > 1 else "relay.rayquaza.ca"
port = int(sys.argv[2]) if len(sys.argv) > 2 else 27420
ip = socket.gethostbyname(host)
print("%s -> %s" % (host, ip))
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(3)
nonce = os.urandom(4)
pkt = b"GBR1" + bytes([1, 10, 0xFF, 0xFF]) + b"\0" * 8 + nonce
pkt += b"\0" * (64 - len(pkt))                 # padded: the server never answers larger
ok = 0
for k in (0, 1):
    s.sendto(pkt, (ip, port + k))
    try:
        d, a = s.recvfrom(256)
        if d[:4] == b"GBR1" and d[5] == 11 and d[16:20] == nonce:
            pub = socket.inet_ntoa(d[20:24])
            pp = struct.unpack("!H", d[24:26])[0]
            print("port %d: PROBED ok, the server sees you as %s:%d" % (port + k, pub, pp))
            ok += 1
        else:
            print("port %d: unexpected reply %r" % (port + k, d[:32]))
    except socket.timeout:
        print("port %d: no answer in 3 s" % (port + k))
print("RELAY REACHABLE" if ok == 2 else "RELAY NOT REACHABLE (%d/2 ports)" % ok)
sys.exit(0 if ok == 2 else 1)

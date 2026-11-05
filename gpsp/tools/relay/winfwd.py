"""winfwd.py -- carry the LAN's GBR1 traffic to a gbadhoc-relay running in WSL2.

WSL2 sits behind Windows' own NAT, so consoles on the LAN cannot reach it.
This listens on the PC's LAN address, ports P and P + 1 (the relay's main and
PROBE ports), and forwards every datagram to `gbadhoc-relay --fwd PORT` framed
as  ip[4] port[2] sock[1] payload, so the relay core still sees each console's
real address; replies come back framed the same way and leave from the
matching LAN socket.  docs/ONLINE-TUNNEL.md section 7 (the LAN test).

    python winfwd.py --lan 10.0.0.40 --wsl 172.23.21.144 [--port 27420] [--fwd 27430]

Bind a LAN address explicitly (never 0.0.0.0): the relay is LAN-only unless the
owner forwards a port on the router himself (builds/online-out/REAL-TEST.md).
"""
import argparse
import select
import socket
import struct
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lan", required=True)
    ap.add_argument("--wsl", required=True)
    ap.add_argument("--port", type=int, default=27420)
    ap.add_argument("--fwd", type=int, default=27430)
    a = ap.parse_args()
    lan = []
    for k in (0, 1):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind((a.lan, a.port + k))
        s.setblocking(False)
        lan.append(s)
    up = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    up.bind(("0.0.0.0", 0))
    up.setblocking(False)
    dest = (a.wsl, a.fwd)
    n_in = n_out = 0
    t_stat = time.time()
    print("winfwd %s:%d/%d <-> %s:%d" % (a.lan, a.port, a.port + 1, a.wsl, a.fwd), flush=True)
    while True:
        r, _, _ = select.select(lan + [up], [], [], 1.0)
        for s in r:
            while True:
                try:
                    data, src = s.recvfrom(2048)
                except (BlockingIOError, ConnectionResetError, OSError):
                    break
                if s is up:
                    if len(data) < 7:
                        continue
                    ip = socket.inet_ntoa(data[0:4])
                    port = struct.unpack("!H", data[4:6])[0]
                    lan[data[6] & 1].sendto(data[7:], (ip, port))
                    n_out += 1
                else:
                    k = lan.index(s)
                    hdr = socket.inet_aton(src[0]) + struct.pack("!HB", src[1], k)
                    up.sendto(hdr + data, dest)
                    n_in += 1
        if time.time() - t_stat > 10:
            print("winfwd in=%d out=%d" % (n_in, n_out), flush=True)
            t_stat = time.time()


if __name__ == "__main__":
    main()

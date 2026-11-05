#!/usr/bin/env python3
"""natlab.py -- the online-play NAT matrix on REAL sockets, no root, no internet.

Run as:   unshare -rn --fork python3 natlab.py BIN_DIR [-v]
(-r maps us to root inside a fresh user namespace; -n gives it an empty network
namespace, which plays "the internet").  Each console and the server get their
own network namespace whose only interface is a TUN device; this script reads
every IPv4/UDP packet from those TUNs and routes it the way the internet and two
home routers would: NAT boxes with endpoint-independent ("cone": EIM mapping,
port-restricted filtering) or symmetric mapping, a LAN inside each, 20 ms per
internet leg.  Nothing leaves the namespace; nothing touches a real interface.

Per case: gbadhoc-relay on 203.0.113.1, two relay_client processes (the desktop
stand-in for a PSP: transport_relay + netdrv on POSIX sockets), each sending
600 reliable payloads.  PASS needs both RESULT ok=1 and the expected path.
docs/ONLINE-TUNNEL.md section 5, "Testing it without the internet" (1).
"""
import fcntl
import heapq
import os
import select
import socket
import struct
import subprocess
import sys
import time

TUNSETIFF = 0x400454CA
IFF_TUN, IFF_NO_PI = 0x0001, 0x1000
LEG_S = 0.020
SERVER = "203.0.113.1"
VERBOSE = "-v" in sys.argv


def sh(*a, ns=None):
    cmd = (["nsenter", "--net=/proc/%d/ns/net" % ns] if ns else []) + list(a)
    subprocess.run(cmd, check=True)


class Node:
    def __init__(self, name, ip, prefix):
        self.name, self.ip = name, ip
        self.proc = subprocess.Popen(["unshare", "-n", "sleep", "3600"])
        time.sleep(0.05)
        pid = self.proc.pid
        sh("ip", "link", "set", "lo", "up", ns=pid)
        sh("ip", "tuntap", "add", "dev", "tun0", "mode", "tun", ns=pid)
        sh("ip", "addr", "add", "%s/%d" % (ip, prefix), "dev", "tun0", ns=pid)
        sh("ip", "link", "set", "tun0", "up", ns=pid)
        sh("ip", "route", "add", "default", "dev", "tun0", ns=pid)
        # open the TUN from inside its namespace (the fd stays bound to it)
        me = os.open("/proc/self/ns/net", os.O_RDONLY)
        it = os.open("/proc/%d/ns/net" % pid, os.O_RDONLY)
        os.setns(it, os.CLONE_NEWNET)
        self.fd = os.open("/dev/net/tun", os.O_RDWR | os.O_NONBLOCK)
        fcntl.ioctl(self.fd, TUNSETIFF, struct.pack("16sH", b"tun0", IFF_TUN | IFF_NO_PI))
        os.setns(me, os.CLONE_NEWNET)
        os.close(me)
        os.close(it)

    def run(self, *argv, **kw):
        return subprocess.Popen(["nsenter", "--net=/proc/%d/ns/net" % self.proc.pid] + list(argv),
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **kw)

    def close(self):
        os.close(self.fd)
        self.proc.kill()
        self.proc.wait()


class Nat:
    def __init__(self, kind, pub, lan):
        self.kind, self.pub, self.lan = kind, pub, lan    # lan = "10.0.N."
        self.maps = {}          # key -> [pub_port, allowed set]
        self.rev = {}           # pub_port -> (key, priv)
        self.next = 40000 + 1000 * int(lan.split(".")[2])

    def out(self, priv, dst):
        key = (priv, dst) if self.kind == "sym" else (priv,)
        if key not in self.maps:
            self.maps[key] = [self.next, set()]
            self.rev[self.next] = (key, priv)
            self.next += 1
        m = self.maps[key]
        m[1].add(dst)
        return (self.pub, m[0])

    def inb(self, src, port):
        if port not in self.rev:
            return None
        key, priv = self.rev[port]
        if src not in self.maps[key][1]:
            return None          # port-restricted filtering
        return priv

    def forget(self):
        self.maps.clear()
        self.rev.clear()


def csum(b):
    if len(b) % 2:
        b += b"\0"
    s = sum(struct.unpack("!%dH" % (len(b) // 2), b))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return ~s & 0xFFFF


def rewrite(pkt, src, dst):
    ihl = (pkt[0] & 15) * 4
    h = bytearray(pkt[:ihl])
    h[12:16] = socket.inet_aton(src[0])
    h[16:20] = socket.inet_aton(dst[0])
    h[10:12] = b"\0\0"
    h[10:12] = struct.pack("!H", csum(bytes(h)))
    u = bytearray(pkt[ihl:])
    u[0:2] = struct.pack("!H", src[1])
    u[2:4] = struct.pack("!H", dst[1])
    u[6:8] = b"\0\0"            # UDP checksum 0 = none (legal over IPv4)
    return bytes(h) + bytes(u)


def parse(pkt):
    if len(pkt) < 28 or pkt[0] >> 4 != 4 or pkt[9] != 17:
        return None
    ihl = (pkt[0] & 15) * 4
    s = socket.inet_ntoa(pkt[12:16])
    d = socket.inet_ntoa(pkt[16:20])
    sp, dp = struct.unpack("!HH", pkt[ihl:ihl + 4])
    return (s, sp), (d, dp)


def run_case(binp, name, kinds, same_lan, expect, drop_ms=0):
    """kinds: (host, guest) each 'cone' | 'sym' | 'none' (public address)"""
    nodes, nats, by_ip = {}, {}, {}
    srv = Node("server", SERVER, 32)
    by_ip[SERVER] = srv
    for i, k in enumerate(kinds):
        lan_n = 0 if same_lan else i
        if k == "none":
            ip = "192.0.2.%d" % (10 + i)
            n = Node("c%d" % i, ip, 32)
            n.nat = None
        else:
            lan = "10.0.%d." % lan_n
            if lan_n not in nats:
                nats[lan_n] = Nat(k, "198.51.100.%d" % (10 + lan_n), lan)
            ip = lan + str(20 + i)
            n = Node("c%d" % i, ip, 24)
            n.nat = nats[lan_n]
        nodes[i] = n
        by_ip[ip] = n
    allnodes = [srv] + list(nodes.values())
    q = []                       # (due, seq, node, packet)
    seq = 0

    def emit(node, pkt, delay):
        nonlocal seq
        seq += 1
        heapq.heappush(q, (time.time() + delay, seq, node, pkt))

    def route(src_node, pkt):
        p = parse(pkt)
        if not p:
            return
        src, dst = p
        nat = getattr(src_node, "nat", None)
        if nat and dst[0].startswith("10."):
            if dst[0].startswith(nat.lan) and dst[0] in by_ip:
                emit(by_ip[dst[0]], pkt, 0.002)     # same LAN
            return
        if nat:
            src = nat.out(src, dst)
            pkt = rewrite(pkt, src, dst)
        # the internet: to a public node, or into a NAT
        if dst[0] in by_ip and not getattr(by_ip[dst[0]], "nat", None):
            emit(by_ip[dst[0]], pkt, LEG_S)
            return
        for nt in nats.values():
            if nt.pub == dst[0]:
                priv = nt.inb(src, dst[1])
                if priv and priv[0] in by_ip:
                    emit(by_ip[priv[0]], rewrite(pkt, src, priv), LEG_S)
                return

    server = srv.run(binp + "/gbadhoc-relay", "--bind", SERVER)
    time.sleep(0.3)
    cl = []
    for i in (0, 1):
        a = [binp + "/relay_client", "--server", SERVER + ":27420", "--host" if i == 0 else "--join",
             "--code", "NATS", "--bind", "0.0.0.0:27500", "--secs", "40"]
        if drop_ms and i == 1:
            a += ["--drop-direct-after", str(drop_ms)]
        cl.append(nodes[i].run(*a))
        time.sleep(0.2)
    out = {0: [], 1: []}
    fdmap = {n.fd: n for n in allnodes}
    pipes = {cl[0].stdout.fileno(): 0, cl[1].stdout.fileno(): 1}
    for f in pipes:
        os.set_blocking(f, False)
    t_end = time.time() + 60
    while time.time() < t_end and any(c.poll() is None for c in cl):
        to = max(0.0, min(0.01, (q[0][0] - time.time()) if q else 0.01))
        r, _, _ = select.select(list(fdmap) + list(pipes), [], [], to)
        for f in r:
            if f in fdmap:
                while True:
                    try:
                        pkt = os.read(f, 2048)
                    except BlockingIOError:
                        break
                    route(fdmap[f], pkt)
            else:
                try:
                    data = os.read(f, 65536).decode(errors="replace")
                except BlockingIOError:
                    data = ""
                out[pipes[f]].append(data)
        now = time.time()
        while q and q[0][0] <= now:
            _, _, node, pkt = heapq.heappop(q)
            try:
                os.write(node.fd, pkt)
            except OSError:
                pass
    for i in (0, 1):
        try:
            rest = cl[i].communicate(timeout=5)[0] or ""
        except subprocess.TimeoutExpired:
            cl[i].kill()
            rest = cl[i].communicate()[0] or ""
        out[i].append(rest)
    server.kill()
    srvlog = server.communicate()[0]
    res = {}
    for i in (0, 1):
        text = "".join(out[i])
        if VERBOSE:
            print("---- c%d\n%s" % (i, text))
        line = [l for l in text.splitlines() if l.startswith("RESULT")]
        res[i] = dict(kv.split("=", 1) for kv in line[-1].split()[1:]) if line else {}
    if VERBOSE:
        print("---- server\n" + srvlog)
    for n in allnodes:
        n.close()
    ok = all(res[i].get("ok") == "1" for i in (0, 1))
    paths = [res[i].get("path", "?") for i in (0, 1)]
    want = {"direct": ("direct",), "relay": ("relay", "fellback")}[expect]
    ok = ok and all(p in want for p in paths)
    print("%s %-30s path=%s/%s nat=%s/%s got=%s/%s rtt_direct=%s/%s rtt_relay=%s/%s srtt=%s/%s secs=%s/%s" % (
        "ok  " if ok else "FAIL", name, paths[0], paths[1], res[0].get("nat"), res[1].get("nat"),
        res[0].get("got"), res[1].get("got"), res[0].get("rtt_direct_ms"), res[1].get("rtt_direct_ms"),
        res[0].get("rtt_relay_ms"), res[1].get("rtt_relay_ms"), res[0].get("srtt_ms"), res[1].get("srtt_ms"),
        res[0].get("secs"), res[1].get("secs")))
    sys.stdout.flush()
    return ok


# ---------------------------------------------------------------------------
# The desktop-host matrix (docs/ONLINE-TUNNEL.md, "Desktop hosts and
# discovery"): the shared server on 203.0.113.1 is the DIRECTORY only; a
# desktop host (gbadhoc-relay in desktop mode: a home code and a registration,
# the same relay_core + relay_dirclient as GBAdhoc XMB) sits behind a cone NAT
# whose UPnP mapping is simulated as two static forwards (27420, 27421); the
# two consoles find it by its four-letter code (LOOKUP at the directory) or by
# its invite code (no server at all) and play through it.

DESK_PUB = "198.51.100.99"
DESK_LAN = "10.0.9."
DESK_IP = DESK_LAN + "2"


class FwdNat(Nat):
    """A NAT with UPnP port forwards: pub_port -> (ip, port), unfiltered; the
    forwarded socket's outbound traffic leaves on its forwarded port."""

    def __init__(self, kind, pub, lan, fwd):
        Nat.__init__(self, kind, pub, lan)
        self.fwd = dict(fwd)
        self.fwd_rev = {v: k for k, v in self.fwd.items()}

    def out(self, priv, dst):
        if priv in self.fwd_rev:
            return (self.pub, self.fwd_rev[priv])
        return Nat.out(self, priv, dst)

    def inb(self, src, port):
        if port in self.fwd:
            return self.fwd[port]
        return Nat.inb(self, src, port)


def run_desktop_case(binp, name, kinds, by, expect):
    """kinds: each PSP 'cone' | 'sym' | 'home' (behind the desktop's router);
    by: each PSP 'code' | 'invite'"""
    nats, by_ip = {}, {}
    srv = Node("dir", SERVER, 32)
    srv.nat = None
    by_ip[SERVER] = srv
    dn = FwdNat("cone", DESK_PUB, DESK_LAN, {27420: (DESK_IP, 27420), 27421: (DESK_IP, 27421)})
    nats[9] = dn
    desk = Node("desk", DESK_IP, 24)
    desk.nat = dn
    by_ip[DESK_IP] = desk
    nodes = {}
    for i, k in enumerate(kinds):
        if k == "home":
            ip = DESK_LAN + str(30 + i)
            n = Node("c%d" % i, ip, 24)
            n.nat = dn
        else:
            lan = "10.0.%d." % i
            nats[i] = Nat(k, "198.51.100.%d" % (10 + i), lan)
            ip = lan + str(20 + i)
            n = Node("c%d" % i, ip, 24)
            n.nat = nats[i]
        nodes[i] = n
        by_ip[ip] = n
    allnodes = [srv, desk] + list(nodes.values())
    q = []
    seq = [0]

    def emit(node, pkt, delay):
        seq[0] += 1
        heapq.heappush(q, (time.time() + delay, seq[0], node, pkt))

    def route(src_node, pkt):
        p = parse(pkt)
        if not p:
            return
        src, dst = p
        nat = src_node.nat
        if nat and dst[0].startswith("10."):
            if dst[0].startswith(nat.lan) and dst[0] in by_ip:
                emit(by_ip[dst[0]], pkt, 0.002)     # same LAN
            return
        if nat and dst[0] == nat.pub:
            return                                 # no NAT loopback (hairpin)
        if nat:
            src = nat.out(src, dst)
            pkt = rewrite(pkt, src, dst)
        if dst[0] in by_ip and not by_ip[dst[0]].nat:
            emit(by_ip[dst[0]], pkt, LEG_S)
            return
        for nt in nats.values():
            if nt.pub == dst[0]:
                priv = nt.inb(src, dst[1])
                if priv and priv[0] in by_ip:
                    emit(by_ip[priv[0]], rewrite(pkt, src, priv), LEG_S)
                return

    fdmap = {n.fd: n for n in allnodes}
    outs, pipes = {}, {}

    def watch(tag, proc):
        os.set_blocking(proc.stdout.fileno(), False)
        pipes[proc.stdout.fileno()] = tag
        outs[tag] = []

    def pump(until, timeout):
        t_end = time.time() + timeout
        while time.time() < t_end and not until():
            to = max(0.0, min(0.01, (q[0][0] - time.time()) if q else 0.01))
            r, _, _ = select.select(list(fdmap) + list(pipes), [], [], to)
            for f in r:
                if f in fdmap:
                    while True:
                        try:
                            pkt = os.read(f, 2048)
                        except BlockingIOError:
                            break
                        route(fdmap[f], pkt)
                else:
                    try:
                        data = os.read(f, 65536).decode(errors="replace")
                    except BlockingIOError:
                        data = ""
                    outs[pipes[f]].append(data)
            now = time.time()
            while q and q[0][0] <= now:
                _, _, node, pkt = heapq.heappop(q)
                try:
                    os.write(node.fd, pkt)
                except OSError:
                    pass

    dirp = srv.run(binp + "/gbadhoc-relay", "--bind", SERVER)
    deskp = desk.run(binp + "/gbadhoc-relay", "--bind", "0.0.0.0", "--home", "QWER",
                     "--register", SERVER + ":27420", "--advertise-port", "27420",
                     "--lan-ip", DESK_IP, "--public", DESK_PUB)
    watch("dir", dirp)
    watch("desk", deskp)
    pump(lambda: "registered" in "".join(outs["desk"]), 8)
    dtext = "".join(outs["desk"])
    invite = [l.split()[1] for l in dtext.splitlines() if l.startswith("invite ")]
    code = [l.split("=")[1].split()[0] for l in dtext.splitlines() if l.startswith("home code=")]
    if "registered" not in dtext or not invite or not code:
        for f in pipes:
            os.set_blocking(f, True)
        print("FAIL %-40s the desktop did not register: %r" % (name, dtext[-300:]))
        for p in (dirp, deskp):
            p.kill()
        for n in allnodes:
            n.close()
        return False
    cl = []
    for i in (0, 1):
        role = "--host" if i == 0 else "--join"
        if by[i] == "code":
            a = [binp + "/relay_client", "--server", SERVER + ":27420", "--lookup", "--code", code[0]]
        else:
            a = [binp + "/relay_client", "--invite", invite[0]]
        a += [role, "--bind", "0.0.0.0:27500", "--secs", "40"]
        cl.append(nodes[i].run(*a))
        watch(i, cl[i])
        pump(lambda: False, 0.2)
    pump(lambda: all(c.poll() is not None for c in cl), 60)
    for f in pipes:
        os.set_blocking(f, True)            # communicate() needs blocking pipes
    res = {}
    for i in (0, 1):
        try:
            rest = cl[i].communicate(timeout=5)[0] or ""
        except subprocess.TimeoutExpired:
            cl[i].kill()
            rest = cl[i].communicate()[0] or ""
        text = "".join(outs[i]) + rest
        if VERBOSE:
            print("---- c%d\n%s" % (i, text))
        line = [l for l in text.splitlines() if l.startswith("RESULT")]
        res[i] = dict(kv.split("=", 1) for kv in line[-1].split()[1:]) if line else {}
    for p in (dirp, deskp):
        p.terminate()
    dirlog = "".join(outs["dir"]) + (dirp.communicate()[0] or "")
    desklog = "".join(outs["desk"]) + (deskp.communicate()[0] or "")
    if VERBOSE:
        print("---- directory\n" + dirlog + "\n---- desktop\n" + desklog)
    for n in allnodes:
        n.close()
    ok = all(res[i].get("ok") == "1" for i in (0, 1))
    paths = [res[i].get("path", "?") for i in (0, 1)]
    want = {"direct": ("direct",), "relay": ("relay", "fellback")}[expect]
    ok = ok and all(p in want for p in paths)
    # the directory never makes a room and never forwards game traffic
    carried = "room_create" in dirlog or "room_join" in dirlog
    ok = ok and not carried
    servers = [res[i].get("server", "?") for i in (0, 1)]
    for i in (0, 1):
        want_srv = (DESK_IP if kinds[i] == "home" and by[i] == "code" else DESK_PUB) + ":27420"
        if servers[i] != want_srv:
            ok = False
    print("%s %-36s path=%s/%s by=%s/%s server=%s/%s nat=%s/%s got=%s/%s invite=%s dir_rooms=%s secs=%s/%s" % (
        "ok  " if ok else "FAIL", name, paths[0], paths[1], by[0], by[1], servers[0], servers[1],
        res[0].get("nat"), res[1].get("nat"), res[0].get("got"), res[1].get("got"), invite[0],
        "yes" if carried else "none", res[0].get("secs"), res[1].get("secs")))
    sys.stdout.flush()
    return ok


SHARED_CASES = [
    ("cone x cone", ("cone", "cone"), False, "direct", 0),
    ("public x cone", ("none", "cone"), False, "direct", 0),
    ("cone x symmetric", ("cone", "sym"), False, "relay", 0),
    ("symmetric x symmetric", ("sym", "sym"), False, "relay", 0),
    ("same LAN, one NAT", ("cone", "cone"), True, "direct", 0),
    ("direct dies at 6 s", ("cone", "cone"), False, "relay", 6000),
]

DESKTOP_CASES = [
    ("desk code: cone x cone", ("cone", "cone"), ("code", "code"), "direct"),
    ("desk code: cone x symmetric", ("cone", "sym"), ("code", "code"), "relay"),
    ("desk code: symmetric x symmetric", ("sym", "sym"), ("code", "code"), "relay"),
    ("desk invite: cone x cone", ("cone", "cone"), ("invite", "invite"), "direct"),
    ("desk invite: symmetric x cone", ("sym", "cone"), ("invite", "invite"), "relay"),
    ("desk code+invite: cone x cone", ("cone", "cone"), ("code", "invite"), "direct"),
    ("desk code: host in the PC's home", ("home", "cone"), ("code", "code"), "relay"),
]


def main():
    """natlab.py BIN_DIR [shared|desktop] [-v]   (default: both matrices)"""
    binp = sys.argv[1]
    which = [a for a in sys.argv[2:] if a in ("shared", "desktop")]
    fails = total = 0
    if not which or "shared" in which:
        for c in SHARED_CASES:
            total += 1
            if not run_case(binp, *c):
                fails += 1
    if not which or "desktop" in which:
        for c in DESKTOP_CASES:
            total += 1
            if not run_desktop_case(binp, *c):
                fails += 1
    print("%s: %d/%d cases" % ("FAILED" if fails else "PASS", total - fails, total))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())

"""upnp_map.py -- open this PC's GBAdhoc online game port on the home router
(UPnP-IGD), so a friend behind a phone's carrier NAT reaches a PPSSPP / PC
host DIRECTLY instead of through the relay.  docs/ONLINE-TUNNEL.md section 5,
"Direct wherever possible".

    python upnp_map.py                 show the router, its public IP, the mappings
    python upnp_map.py add [PORT]      UDP PORT (default 27500) -> this PC:PORT
    python upnp_map.py del [PORT]

Why it works with no setting in GBAdhoc: the relay offers each console's
public IP with its own socket port (27500) as an extra candidate, so once the
router forwards that port, the friend's punches arrive.  A different external
port needs the host to say so (map_port; relay_client --map-port).

PPSSPP: GBAdhoc's socket is PPSSPP's own UDP 27500 on this PC (the first
instance; a second one gets another port).  The lease is permanent: run
`del` when done.  Never maps anything but UDP PORT to this PC.
"""
import re
import socket
import sys
import urllib.error
import urllib.parse
import urllib.request
from xml.etree import ElementTree as ET

ST = ["urn:schemas-upnp-org:device:InternetGatewayDevice:1",
      "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
      "urn:schemas-upnp-org:service:WANIPConnection:1"]


def lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))      # no packet: just the route's source address
        return s.getsockname()[0]
    finally:
        s.close()


def ssdp(me):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(me))
    s.bind((me, 0))
    s.settimeout(3)
    for st in ST:
        s.sendto(("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\n"
                  "MX: 2\r\nST: %s\r\n\r\n" % st).encode(), ("239.255.255.250", 1900))
    locs = set()
    try:
        while True:
            d, _ = s.recvfrom(4096)
            m = re.search(rb"(?im)^location:\s*(\S+)", d)
            if m:
                locs.add(m.group(1).decode())
    except socket.timeout:
        pass
    return locs


def find_ctrl(loc):
    root = ET.fromstring(urllib.request.urlopen(loc, timeout=5).read())
    for svc in root.iter():
        if svc.tag.endswith("service"):
            t = c = None
            for ch in svc:
                if ch.tag.endswith("serviceType"):
                    t = ch.text
                if ch.tag.endswith("controlURL"):
                    c = ch.text
            if t and ("WANIPConnection" in t or "WANPPPConnection" in t):
                return t, urllib.parse.urljoin(loc, c)
    return None, None


def soap(url, st, action, args):
    body = "".join("<%s>%s</%s>" % (k, v, k) for k, v in args)
    env = ('<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
           's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body>'
           '<u:%s xmlns:u="%s">%s</u:%s></s:Body></s:Envelope>' % (action, st, body, action))
    req = urllib.request.Request(url, env.encode(), {"Content-Type": 'text/xml; charset="utf-8"',
                                                    "SOAPAction": '"%s#%s"' % (st, action)})
    try:
        return 200, urllib.request.urlopen(req, timeout=5).read().decode(errors="replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode(errors="replace")


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "show"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 27500
    me = lan_ip()
    locs = ssdp(me)
    if not locs:
        print("no UPnP router answered (UPnP off, or not a home router): the relay it is")
        return 1
    for loc in sorted(locs):
        try:
            st, url = find_ctrl(loc)
        except Exception as e:     # noqa: BLE001 -- a broken device description
            print("  %s: %s" % (loc, e))
            continue
        if not url:
            continue
        code, r = soap(url, st, "GetExternalIPAddress", [])
        m = re.search(r"<NewExternalIPAddress>(.*?)<", r)
        pub = m.group(1) if m else "?"
        print("router %s  public IP %s  this PC %s" % (url, pub, me))
        if pub.startswith("100.") and 64 <= int(pub.split(".")[1]) < 128:
            print("NOTE: %s is carrier-grade NAT space: a forward here cannot be reached" % pub)
        if cmd == "show":
            for i in range(64):
                code, r = soap(url, st, "GetGenericPortMappingEntry", [("NewPortMappingIndex", i)])
                if code != 200:
                    break
                f = dict(re.findall(r"<(New\w+)>(.*?)</\1>", r))
                print("  map %s %s -> %s:%s  %s" % (f.get("NewProtocol"), f.get("NewExternalPort"),
                      f.get("NewInternalClient"), f.get("NewInternalPort"),
                      f.get("NewPortMappingDescription")))
        elif cmd == "add":
            code, r = soap(url, st, "AddPortMapping", [
                ("NewRemoteHost", ""), ("NewExternalPort", port), ("NewProtocol", "UDP"),
                ("NewInternalPort", port), ("NewInternalClient", me), ("NewEnabled", 1),
                ("NewPortMappingDescription", "GBAdhoc online %d" % port), ("NewLeaseDuration", 0)])
            print("add UDP %d -> %s:%d: %s" % (port, me, port, "ok" if code == 200 else code))
        elif cmd == "del":
            code, r = soap(url, st, "DeletePortMapping", [
                ("NewRemoteHost", ""), ("NewExternalPort", port), ("NewProtocol", "UDP")])
            print("del UDP %d: %s" % (port, "ok" if code == 200 else code))
        else:
            print(__doc__)
            return 2
        return 0
    print("no router with a WANIPConnection service")
    return 1


if __name__ == "__main__":
    sys.exit(main())

"""ddns_cloudflare.py -- keep relay.rayquaza.ca's A record on this home's public IPv4.

NOT run by anything automatically.  The owner supplies a Cloudflare API token
scoped to Zone.DNS:Edit for rayquaza.ca only, in the environment:

    set CF_API_TOKEN=...            (never commit it, never pass it on a command line in a log)
    python ddns_cloudflare.py [--name relay.rayquaza.ca] [--zone rayquaza.ca] [--dry-run]

It reads the public IPv4 (Cloudflare's own https://1.1.1.1/cdn-cgi/trace), compares it
with the record, and updates the record only when it differs.  The record stays
DNS-only (proxied = false): Cloudflare's proxy does not carry UDP.  Run it from Task
Scheduler every 10 minutes once the owner has a token.
"""
import argparse
import json
import os
import sys
import urllib.request

API = "https://api.cloudflare.com/client/v4"


def call(method, path, token, body=None):
    r = urllib.request.Request(API + path, method=method,
                               data=json.dumps(body).encode() if body is not None else None,
                               headers={"Authorization": "Bearer " + token, "Content-Type": "application/json"})
    with urllib.request.urlopen(r, timeout=20) as f:
        d = json.loads(f.read())
    if not d.get("success"):
        raise SystemExit("cloudflare: %s" % d.get("errors"))
    return d["result"]


def public_ip():
    with urllib.request.urlopen("https://1.1.1.1/cdn-cgi/trace", timeout=10) as f:
        for line in f.read().decode().splitlines():
            if line.startswith("ip="):
                return line[3:].strip()
    raise SystemExit("no ip= in the trace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", default="relay.rayquaza.ca")
    ap.add_argument("--zone", default="rayquaza.ca")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    token = os.environ.get("CF_API_TOKEN")
    if not token:
        sys.exit("CF_API_TOKEN is not set (the owner's token, Zone.DNS:Edit on %s only)" % a.zone)
    ip = public_ip()
    if ":" in ip:
        sys.exit("public address %s is IPv6; the relay needs the IPv4 A record" % ip)
    zone = call("GET", "/zones?name=" + a.zone, token)
    if not zone:
        sys.exit("zone %s not found for this token" % a.zone)
    zid = zone[0]["id"]
    recs = call("GET", "/zones/%s/dns_records?type=A&name=%s" % (zid, a.name), token)
    body = {"type": "A", "name": a.name, "content": ip, "ttl": 120, "proxied": False}
    if recs and recs[0]["content"] == ip and not recs[0].get("proxied"):
        print("%s already %s" % (a.name, ip))
        return
    if a.dry_run:
        print("would set %s -> %s (was %s)" % (a.name, ip, recs[0]["content"] if recs else "none"))
        return
    if recs:
        call("PUT", "/zones/%s/dns_records/%s" % (zid, recs[0]["id"]), token, body)
    else:
        call("POST", "/zones/%s/dns_records" % zid, token, body)
    print("%s -> %s (DNS only)" % (a.name, ip))


if __name__ == "__main__":
    main()

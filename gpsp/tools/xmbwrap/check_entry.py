#!/usr/bin/env python3
"""check_entry.py -- check one xmbwrap entry (test_xmbwrap.sh).

  check_entry.py TEMPLATE.PBP DIR TITLE ROM GBADHOC ICON_WxH|- PIC1_WxH|-
  check_entry.py --sfo-ref REF.SFO ENTRY/EBOOT.PBP     (PARAM.SFO byte-identical)
"""
import struct, sys, zlib


def pbp(path):
    d = open(path, "rb").read()
    assert d[:4] == b"\0PBP", "not a PBP"
    offs = list(struct.unpack("<8I", d[8:40])) + [len(d)]
    return [d[offs[i]:offs[i + 1]] for i in range(8)]


def sfo(d):
    assert d[:4] == b"\0PSF"
    kt, dt, n = struct.unpack("<III", d[8:20])
    out = {}
    for i in range(n):
        ko, fmt, ln, mx, do = struct.unpack("<HHIII", d[20 + 16 * i:36 + 16 * i])
        key = d[kt + ko:d.index(b"\0", kt + ko)].decode()
        v = d[dt + do:dt + do + ln]
        out[key] = (fmt, v.rstrip(b"\0").decode("utf-8") if fmt == 0x0204 else struct.unpack("<I", v)[0], mx)
    return out


def png_size(b):
    if not b:
        return "-"
    assert b[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    w, h = struct.unpack(">II", b[16:24])
    return "%dx%d" % (w, h)


def fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


if sys.argv[1] == "--sfo-ref":
    ref = open(sys.argv[2], "rb").read()
    got = pbp(sys.argv[3])[0]
    if got != ref:
        fail("PARAM.SFO differs from mksfoex (%d vs %d bytes): %r vs %r" % (len(got), len(ref), sfo(got), sfo(ref)))
    print("ok: PARAM.SFO == mksfoex,", sfo(got)["TITLE"][1])
    sys.exit(0)

tpl, d, title, rom, gb, icon, pic = sys.argv[1:8]
t = pbp(tpl)
e = pbp(d + "/EBOOT.PBP")
if e[6] != t[6]:
    fail("DATA.PSP is not the launcher's")
st, se = sfo(t[0]), sfo(e[0])
if se["TITLE"][1] != title:
    fail("TITLE %r, want %r" % (se["TITLE"][1], title))
for k in st:
    if k != "TITLE" and st[k] != se[k]:
        fail("SFO key %s changed: %r -> %r" % (k, st[k], se[k]))
if png_size(e[1]) != icon:
    fail("ICON0 %s, want %s" % (png_size(e[1]), icon))
if png_size(e[4]) != pic:
    fail("PIC1 %s, want %s" % (png_size(e[4]), pic))
for i in (2, 3, 5, 7):
    if e[i]:
        fail("section %d not empty" % i)
ini = open(d + "/launch.ini", encoding="utf-8").read()
kv = dict(l.split(" = ", 1) for l in ini.splitlines() if " = " in l and not l.startswith("#"))
if kv.get("rom") != rom or kv.get("gbadhoc") != gb:
    fail("launch.ini %r" % kv)
print("ok: %s  title=%r icon=%s pic1=%s rom=%r" % (d.rsplit("/", 1)[-1], title, icon, pic, rom))

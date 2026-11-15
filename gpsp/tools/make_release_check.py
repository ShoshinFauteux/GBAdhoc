#!/usr/bin/env python3
# make_release_check.py MANIFEST EBOOT ME_PRX gpsp_features.h psp/Makefile
# Called by make_release.sh: refuses any build that is not the release set.
# Text inputs are read as UTF-8 with errors replaced (psp/Makefile carries a
# cp1252 dash in a comment; every pattern matched here is ASCII).
import hashlib, json, pathlib, re, sys
m = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()
if m.get("profile") != "release":
    raise SystemExit("manifest profile is not release")
if m.get("eboot", {}).get("sha256") != sha(sys.argv[2]):
    raise SystemExit("EBOOT hash differs from build manifest")
if m.get("mePrxSha256") != sha(sys.argv[3]):
    raise SystemExit("Media Engine PRX hash differs from build manifest")
# THE RIGHT BUILD, not merely a matching one.  A make line that drops a feature,
# or an instrument that slipped in, still says profile=release and hashes fine.
features = pathlib.Path(sys.argv[4]).read_text(encoding="utf-8", errors="replace")
want = {"%s=%s" % (n, v) for n, v in re.findall(r"^#define (\w+) (\S+)$", features, re.M)}
# 20 since code-health A-6 baked 51 (7 oracle what-ifs + 13 the desktop compiles);
# fewer means the header was not parsed.
if len(want) < 7:
    raise SystemExit("gpsp_features.h lists only %d features" % len(want))
want |= {"GPSP_PROFILE=release", "GBA_LINK=1", "REWIND=1"}
have = set(m.get("coreFlags", "").split())
# RFU_SCHED=1 (scheduled adapter ticks, a beta switch) is allowed when both makes
# had it; it changes the adapter's wire format, so the notes must say so.
ticks = "RFU_SCHED=1" in have and "RFU_SCHED=1" in m.get("pspMakeFlags", "").split()
if ticks:
    want.add("RFU_SCHED=1")
if have != want:
    raise SystemExit("coreFlags are not the release feature set: missing %s, extra %s"
                     % (sorted(want - have), sorted(have - want)))
fe = {"-DGPSP_PLAYABLE", "-DGPSP_PROFILE_RELEASE=1"}
fe |= {"-D%s=%s" % (n, v) for n, v in re.findall(r"^#define (\w+) (\S+)$",
                                                 pathlib.Path(sys.argv[5]).with_name("fe_features.h").read_text(encoding="utf-8", errors="replace"), re.M)}
if set(m.get("frontendDefines", "").split()) != fe:
    raise SystemExit("frontendDefines are not the release set: %s" % m.get("frontendDefines"))
psp = set(m.get("pspMakeFlags", "").split())
need = {"ME_CATCH=1", "PSP_EBOOT_ICON1=assets/gbadhoc_icon1.pmf", "GBA_LINK=1", "REWIND=1", "HOT_DATA_GUARD=1"}
# ONLINE=1 is baked (tools/build.sh, flags.lock): the release carries online play,
# off in Settings until the player turns it on (docs/ONLINE-TUNNEL.md).
need.add("ONLINE=1")
# MGIFT=1 is baked too (tools/build.sh default, flags.lock): Mystery Gift ships.
need.add("MGIFT=1")
if ticks:
    need.add("RFU_SCHED=1")
if psp != need:
    raise SystemExit("pspMakeFlags are not the release set: missing %s, extra %s"
                     % (sorted(need - psp), sorted(psp - need)))
pad = re.search(r"^LAYOUT_PAD_BYTES \?= (\d+)", pathlib.Path(sys.argv[5]).read_text(encoding="utf-8", errors="replace"), re.M)
if pad is None or m.get("layoutPin") != 1 or m.get("layoutPadBytes") != int(pad.group(1)):
    raise SystemExit("layout is not the release layout: LAYOUT_PIN=%s LAYOUT_PAD_BYTES=%s"
                     % (m.get("layoutPin"), m.get("layoutPadBytes")))
# THE LOCKED LAYOUT (critique-8 MF2).  tools/build.sh enforces layout.lock only
# for the default switch set (RFU_SCHED=1 among them); a release built any other
# way records "release:differs" and would ship an unchecked layout.  A stable
# RFU_SCHED=0 release re-pins and records its own lock first
# (docs/LAYOUT-PINNING.md, "Keeping the pads").
if m.get("layoutLock") != "release:ok":
    raise SystemExit("layout.lock [release] did not pass: layoutLock=%r (docs/LAYOUT-PINNING.md)"
                     % m.get("layoutLock"))
if m.get("title") != "GBAdhoc" or m.get("cleanRebuild") is not True:
    raise SystemExit("not a clean build titled GBAdhoc: title=%r cleanRebuild=%r"
                     % (m.get("title"), m.get("cleanRebuild")))

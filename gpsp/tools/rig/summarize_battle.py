#!/usr/bin/env python3
"""summarize_battle.py -- score ONE run of the double-battle rig, or refuse to.

    python tools/rig/summarize_battle.py HOST.log JOIN.log
        [--host-ini H.ini --join-ini J.ini]      # what was staged (I4)
        [--host-fixture F --join-fixture F]      # script bytes (I5)
        [--expect-crc CRC]                       # staged EBOOT CRC32 (I3)
        [--host-model PSP-3000 --join-model PSP-1000]   (I8)
        [--band MS_PER_TURN] [--json OUT.json]

docs/RIG-DOUBLE-BATTLE.md is the specification; section numbers below refer
to it.  The rules this file exists to enforce:

  * A run that violates any invariant (I1-I11) is INVALID and is never scored;
    the reasons are printed.  A failure of the GAME (link error, timeout) is a
    valid OUTCOME and is scored as a FAIL with its type.
  * A missing line is UNKNOWN, never zero and never "did not happen".
  * Extremes and changes, never averages of averages (HARNESS §6).  Per-turn
    values are keyed by the engine's own it= iteration, never by line counts.
  * The two consoles' clocks are independent: every latency is a difference
    of two stamps on the SAME console.  Resolution is one emulated frame
    (1000/fps ms, from the run's own session_pace line) per stamp, stated with
    every latency printed.
  * DESYNC (host and join disagree about the battle state at a turn boundary)
    is its own finding and is never folded into lag.
"""
import argparse
import json
import re
import struct
import sys
import zlib

TURN_MARKS = ("t1", "t2_fight", "t3_movemenu", "t4_move", "t5", "t6_fight",
              "t7_movemenu", "t8_move")
KV = re.compile(r"(\w+)=(\S+)")


def kv(line):
    return dict(KV.findall(line))


class Log:
    def __init__(self, path):
        self.path = path
        self.lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
        self.evt = [l[4:] for l in self.lines if l.startswith("EVT ")]

    def all(self, name):
        p = name + " "
        return [kv(e[len(p):]) for e in self.evt if e.startswith(p) or e == name]

    def first(self, name):
        a = self.all(name)
        return a[0] if a else None

    def has(self, name):
        return any(e == name or e.startswith(name + " ") for e in self.evt)


def crc32_file(path):
    return "%08x" % (zlib.crc32(open(path, "rb").read()) & 0xFFFFFFFF)


def ini_pairs(path):
    out = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.split("#")[0].split(";")[0]
        if "=" in line:
            k, v = line.split("=", 1)
            if k.strip():
                out[k.strip()] = v.strip()
    return out


def slope(xs, ys):
    n = len(xs)
    if n < 3:
        return None
    mx, my = sum(xs) / n, sum(ys) / n
    den = sum((x - mx) ** 2 for x in xs)
    return None if den == 0 else sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / den


def median(v):
    v = sorted(v)
    if not v:
        return None
    m = len(v) // 2
    return v[m] if len(v) % 2 else (v[m - 1] + v[m]) / 2


# ------------------------------------------------------------ per console --
def turns(log):
    """it -> {mark: (f, t_ms)}.  The post-loop boundary (it=0 t1 after the
    last turn) becomes turn N+1's t1.  Duplicates are an I7 violation."""
    per, dup, bad_mono = {}, [], []
    last_f = last_t = -1
    post = None
    for m in log.all("ap_mark"):
        name = m.get("text")
        try:
            f, t, it = int(m["f"]), int(m["t_ms"]), int(m.get("it", "0"))
        except (KeyError, ValueError):
            continue
        if f < last_f or t < last_t:
            bad_mono.append(name)
        last_f, last_t = f, t
        if name not in TURN_MARKS:
            continue
        if it == 0:
            if name == "t1":
                post = (f, t)
            continue
        d = per.setdefault(it, {})
        if name in d:
            dup.append((it, name))
        d[name] = (f, t)
    if post and per:
        per.setdefault(max(per) + 1, {})["t1"] = post
    return per, dup, bad_mono


ORDERS = ["GAEM", "GAME", "GEAM", "GEMA", "GMAE", "GMEA",
          "AGEM", "AGME", "AEGM", "AEMG", "AMGE", "AMEG",
          "EGAM", "EGMA", "EAGM", "EAMG", "EMGA", "EMAG",
          "MGAE", "MGEA", "MAGE", "MAEG", "MEGA", "MEAG"]
MONS = ("m0", "m1", "e0", "e1")


def decode_mon(khex, dhex, hp):
    """Party mon as the console holds it: (personality, species, moves, pp, hp).
    k = personality+otId (LE), d = the 48 encrypted substructure bytes."""
    import struct
    kb, db = bytes.fromhex(khex), bytes.fromhex(dhex)
    if len(kb) != 8 or len(db) != 48:
        return None
    pers, otid = struct.unpack("<II", kb)
    if pers == 0 and otid == 0:
        return None
    key = pers ^ otid
    dec = struct.pack("<12I", *[w ^ key for w in struct.unpack("<12I", db)])
    order = ORDERS[pers % 24]
    g, a = order.index("G") * 12, order.index("A") * 12
    species = struct.unpack_from("<H", dec, g)[0]
    moves = struct.unpack_from("<4H", dec, a)
    pp = tuple(dec[a + 8:a + 12])
    return ("%08x" % pers, species, moves, pp, hp)


def fingerprints(log):
    """it -> {personality: (species, moves, pp, hp)} from the party copies.
    RIG §2.9: the member console does not run the battle engine, so the shared
    state is each console's party copy of all four mons.  The post-loop set
    (it=0 after the last turn) becomes N+1."""
    raw = {}
    for v in log.all("ap_val"):
        n = v.get("name", "")
        it = int(v.get("it", "0"))
        for m in MONS:
            if n == "k" + m and "hex" in v:
                raw.setdefault(it, {}).setdefault(m, {})["k"] = v["hex"]
            elif n in ("d%sa" % m, "d%sb" % m) and "hex" in v:
                raw.setdefault(it, {}).setdefault(m, {})[n[-1]] = v["hex"]
            elif n == "h" + m and "val" in v:
                raw.setdefault(it, {}).setdefault(m, {})["h"] = int(v["val"], 16)
    fp = {}
    for it, ms in raw.items():
        out = {}
        for m, d in ms.items():
            if all(k in d for k in ("k", "a", "b", "h")):
                r = decode_mon(d["k"], d["a"] + d["b"], d["h"])
                if r:
                    out[r[0]] = r[1:]
        fp[it] = out
    if 0 in fp and len(fp) > 1:
        post = fp.pop(0)
        fp[max(fp) + 1] = post
    return fp, {}


def console(log, role, args):
    r = {"role": role, "invalid": [], "unknown": []}
    ex = log.first("exit")
    r["exit"] = ex
    per, dup, bad_mono = turns(log)
    complete = [t for t in sorted(per) if all(k in per[t] for k in TURN_MARKS)]
    # turns_completed: consecutive from 1 with the full T1..T8 sequence
    n = 0
    while n + 1 in complete:
        n += 1
    r["turns_completed"] = n
    r["turns_done"] = any(m.get("text") == "turns_done" for m in log.all("ap_mark"))
    if dup:
        r["invalid"].append("I7 duplicate turn marks %s" % dup[:3])
    if bad_mono:
        r["invalid"].append("I7 f/t_ms not monotonic at %s" % bad_mono[:3])
    for t in complete:
        seq = [per[t][k][0] for k in TURN_MARKS]
        if seq != sorted(seq):
            r["invalid"].append("I7 turn %d marks out of order" % t)
    f2m = {"L": [], "R": []}
    turn_ms, period = [], []
    for t in complete:
        d = per[t]
        f2m["L"].append((t, d["t3_movemenu"][1] - d["t2_fight"][1],
                         d["t3_movemenu"][0] - d["t2_fight"][0]))
        f2m["R"].append((t, d["t7_movemenu"][1] - d["t6_fight"][1],
                         d["t7_movemenu"][0] - d["t6_fight"][0]))
        nxt = per.get(t + 1, {}).get("t1")
        if nxt:
            turn_ms.append((t, nxt[1] - d["t8_move"][1]))
            period.append((t, nxt[1] - d["t1"][1]))
    r["f2m"] = f2m
    r["turn_ms"] = turn_ms
    r["period_ms"] = period

    def stats(series):
        if not series:
            return None
        ts = [s[0] for s in series]
        ms = [s[1] for s in series]
        tail = [s for s in series if s[0] >= 3]
        return {
            "n": len(series),
            "first3_median_ms": median(ms[:3]),
            "last3_median_ms": median(ms[-3:]),
            "max_ms": max(ms), "max_turn": ts[ms.index(max(ms))],
            "slope_ms_per_turn": slope([s[0] for s in tail], [s[1] for s in tail]),
        }
    r["f2m_L"] = stats(f2m["L"])
    r["f2m_R"] = stats(f2m["R"])
    r["turn"] = stats(turn_ms)

    # link extremes and changes
    def ints(name, key):
        out = []
        for v in log.all(name):
            try:
                out.append(int(v[key]))
            except (KeyError, ValueError):
                pass
        return out
    link = {}
    for name, key in (("rfu_backlog", "hi"), ("rfu_hbacklog", "hi"),
                      ("rfu_answin", "max_ms"), ("net_stats", "srtt_us"),
                      ("rfu_gqpeak", "peak")):
        v = ints(name, key)
        link[name + "." + key + ".max"] = max(v) if v else "unknown"
    for name, key in (("rfu_backlog", "now"), ("rfu_answin", "mean_ms")):
        v = ints(name, key)
        link[name + "." + key + ".first_last"] = (v[0], v[-1]) if v else "unknown"
    for name, key in (("rfu_shed", "n"), ("rfu_pacedrop", "n"),
                      ("romload", "pg"), ("frame_hist", "late"),
                      ("frame_hist", "forgive")):
        v = ints(name, key)
        link[name + "." + key + ".total"] = sum(v) if v else "unknown"
    ra = [v.get("retx_age", "") for v in log.all("net_stats")]
    ages = [int(a.split("/")[1]) for a in ra if "/" in a]
    link["net_stats.retx_age.max_ms"] = max(ages) if ages else "unknown"
    emp = [(int(v["n"].split("/")[0]), int(v["n"].split("/")[1]))
           for v in log.all("rfu_empty") if "/" in v.get("n", "")]
    link["rfu_empty.share"] = ("%.2f" % (sum(a for a, _ in emp) / max(1, sum(b for _, b in emp)))
                               if emp else "unknown")
    link["session_pace_miss.count"] = len(log.all("session_pace_miss"))
    link["rfu_qdrop.count"] = len(log.all("rfu_qdrop"))
    link["arq_retx_pct.median"] = arq_retx_median(log)
    fps = [v.get("fps") for v in log.all("session_pace") if v.get("fps")]
    r["session_fps"] = float(fps[0]) if fps else None
    r["link"] = link

    # failure type (§2.6), from lines that exist on hardware
    cbs = [v.get("val") for v in log.all("ap_val")]
    probe = " ".join(" ".join("%s=%s" % kv_ for kv_ in v.items()) for v in log.all("fail_probe"))
    lerr = ("030030f4=800ace9" in probe.lower() or "030030f4=800af41" in probe.lower()
            or log.has("rfu_discans"))
    transport = log.has("net_error") or any(
        v for v in log.all("session_stop") if v.get("reason", "0") != "0" and not r["turns_done"])
    if r["turns_done"] and n >= args.turns:
        r["failure"] = None
    elif ex is None:
        r["failure"] = "CRASH"
    elif not any(m.get("text") == "battle_started" for m in log.all("ap_mark")):
        r["failure"] = "SETUP"
    elif lerr:
        r["failure"] = "LINK_ERROR"
    elif transport:
        r["failure"] = "TRANSPORT"
    else:
        r["failure"] = "LAG_TIMEOUT"
    r["fail_line"] = log.first("ap_fail")
    r["fail_probe"] = probe or None
    del cbs
    return r


def arq_retx_median(log):
    """RADIO QUALITY, per console and alignment-free: the transport's own
    retransmissions per packet sent, in windows of >= 500 sends, median over
    the run.  H0 (2026-09-26): the three runs that died before the colosseum
    (4 L, 5 S, 6 B, back to back) sat at 28-64 % on the join; every other run
    at <= 2.4 %.  The MEDIAN, not the worst window: a ratchet collapse (run 7)
    stalls the transport at the very end and spikes the last window to 38 %
    on healthy air -- that is the failure being measured, not bad radio."""
    pts = []
    for v in log.all("net_stats"):
        try:
            pts.append((int(v["tx"]), int(v["retx"])))
        except (KeyError, ValueError):
            pass
    rates, i = [], 0
    while i < len(pts):
        k = i + 1
        while k < len(pts) and pts[k][0] - pts[i][0] < 500:
            k += 1
        if k < len(pts):
            rates.append(100.0 * (pts[k][1] - pts[i][1]) / (pts[k][0] - pts[i][0]))
        i = k
    return round(median(rates), 1) if rates else "unknown"


RADIO_LOSS_PCT = 10.0      # air loss, either direction, that marks a run RADIO


def air_loss(tx_log, rx_log):
    """AIR LOSS of one direction, from both consoles' own counters: the
    sender's retransmissions minus the receiver's DUPLICATES, over the
    sender's sends.  A duplicate is a retransmission whose original had
    already arrived -- it was spurious, forced by an RTO shorter than the real
    round trip, which is exactly what injected latency (arm L) produces.
    Subtracting it leaves the retransmissions the air made necessary.
    (H0b, 2026-09-26: the first version counted raw retransmits and voided an
    L run whose retransmits were 57 % spurious.)  Final cumulative counters;
    None if either side has none."""
    def last(lg):
        v = lg.all("net_stats")
        try:
            return {k: int(v[-1][k]) for k in ("tx", "retx", "dup")} if v else None
        except (KeyError, ValueError):
            return None
    a, b = last(tx_log), last(rx_log)
    if not a or not b or not a["tx"]:
        return None
    return round(100.0 * max(0, a["retx"] - b["dup"]) / a["tx"], 1)


# ------------------------------------------------------------- invariants --
def invariants(h, j, H, J, args):
    bad = []
    for lg, role in ((h, "host"), (j, "join")):
        if not lg.has("boot_ok"):
            bad.append("I1 %s: no boot_ok" % role)
        if not lg.has("exit"):
            bad.append("I1 %s: no exit line (truncated or crashed)" % role)
    rid = [lg.first("rig") for lg in (h, j)]
    if None in rid:
        bad.append("I2 run_id/arm line missing")
    elif (rid[0].get("run_id"), rid[0].get("arm")) != (rid[1].get("run_id"), rid[1].get("arm")) \
            or rid[0].get("run_id") in (None, "none"):
        bad.append("I2 run_id/arm differ or unset: %s vs %s" % (rid[0], rid[1]))
    crc = [lg.first("build") for lg in (h, j)]
    crcs = []
    for lg in (h, j):
        c = [v.get("eboot_crc") for v in lg.all("build") if "eboot_crc" in v]
        crcs.append(c[0] if c else None)
    if None in crcs:
        bad.append("I3 eboot_crc missing on %s" % ("host" if crcs[0] is None else "join"))
    elif crcs[0] != crcs[1]:
        bad.append("I3 eboot_crc differ %s vs %s" % tuple(crcs))
    elif args.expect_crc and crcs[0].lower() != args.expect_crc.lower():
        bad.append("I3 eboot_crc %s != staged %s" % (crcs[0], args.expect_crc))
    del crc
    for lg, role, ini in ((h, "host", args.host_ini), (j, "join", args.join_ini)):
        unk = [v.get("key") for v in lg.all("cfg_unknown") if v.get("src") == "harness"]
        if unk:
            bad.append("I4 %s: harness key(s) nothing read: %s" % (role, unk))
        dup = [v.get("key") for v in lg.all("cfg_duplicate") if v.get("src") == "harness"]
        if dup:
            bad.append("I4 %s: duplicate harness key(s), later line ignored: %s" % (role, dup))
        if not lg.has("cfg_audit"):
            bad.append("I4 %s: no cfg_audit (unknown whether keys applied)" % role)
        if ini:
            seen = {}
            for v in lg.all("cfg"):
                if v.get("src") == "harness":
                    seen.setdefault(v.get("key"), v.get("raw"))   # first wins, like the build
            for k, v in ini_pairs(ini).items():
                if seen.get(k) != v:
                    bad.append("I4 %s: staged %s=%s, log shows %s" % (role, k, v, seen.get(k)))
            for key in ("rfu_shed_keep", "rfu_hold"):
                want = ini_pairs(ini).get(key)
                got = lg.first(key)
                if want is not None and (not got or got.get("n") != want):
                    bad.append("I4 %s: %s staged %s applied %s" % (role, key, want, got))
    for lg, role, fx in ((h, "host", args.host_fixture), (j, "join", args.join_fixture)):
        al = lg.first("ap_loaded")
        if not al:
            bad.append("I5 %s: no ap_loaded" % role)
        elif fx and al.get("crc") != crc32_file(fx):
            bad.append("I5 %s: script crc %s != staged %s" % (role, al.get("crc"), crc32_file(fx)))
    for lg, role in ((h, "host"), (j, "join")):
        if lg.has("evt_gap"):
            bad.append("I6 %s: log lines dropped (%s)" % (role, lg.all("evt_gap")[-1]))
        d = lg.first("evt_drop")
        if d is None:
            bad.append("I6 %s: no evt_drop total (log loss unknown)" % role)
        elif d.get("total") != "0":
            bad.append("I6 %s: evt_drop total=%s" % (role, d.get("total")))
        # rfu_trace_lost is handled after the verdict: see trace_incomplete.
    for r, role in ((H, "host"), (J, "join")):
        bad += ["%s: %s" % (role, x) for x in r["invalid"]]
    # I12 RADIO: a run on bad air is not evidence about either arm.  Measured
    # as AIR loss (retransmits the receiver did not already have), so an arm
    # that injects latency cannot void itself with its own spurious resends.
    for name, a, b in (("host->join", h, j), ("join->host", j, h)):
        m = air_loss(a, b)
        if m is not None and m > RADIO_LOSS_PCT:
            bad.append("I12 %s: RADIO -- air loss %.1f%% > %.0f%%" % (name, m, RADIO_LOSS_PCT))
    # I13 (walk): the colosseum walk never reached its tile.  In the link room
    # a console's OWN movement comes back through the link, so under a long
    # round trip the autopilot keeps the d-pad down past the tile (H0b run 1:
    # the member overshot UP by two tiles and was blocked at x=15 going right,
    # both consoles agreeing).  That is the rig's walk, not the arm's result.
    # Not when link data was discarded: then the walk failed BECAUSE the link
    # lost its key frames, and that is a real failure (H0b run 2, S: pacedrop 55).
    discarded = any(r["link"]["rfu_pacedrop.n.total"] not in (0, "unknown") or
                    r["link"]["rfu_qdrop.count"] for r in (H, J))
    for lg, role in ((h, "host"), (j, "join")):
        fl = lg.first("ap_fail")
        marks = {m.get("text") for m in lg.all("ap_mark")}
        # A walk that overshot: the old holdmash timing out, or the timed-hold
        # walk's settle check (the mark just before it is walk_settle) finding
        # the avatar moved on.  A walk still WAITING for its tile when the
        # 900-frame wait expires is the link not moving it -- a real failure.
        seq = [m.get("text") for m in lg.all("ap_mark")]
        settle = bool(seq) and seq[-1] == "walk_settle"
        if (not discarded and fl and (fl.get("op") == "holdram" or
                                      (fl.get("op") == "waitram" and settle))
                and "in_colosseum" in marks and "on_spot" not in marks):
            bad.append("I13 %s: the colosseum walk did not reach its tile (holdram at line %s): "
                       "the rig's walk, not the link" % (role, fl.get("line")))
    for lg, role, want in ((h, "host", args.host_model), (j, "join", args.join_model)):
        m = lg.first("psp_model")
        if want and (not m or m.get("name") != want):
            bad.append("I8 %s: psp_model %s, expected %s" % (role, m, want))
        up = lg.first("net_up")
        if up and up.get("role") != role:
            bad.append("I8 %s: log says role=%s" % (role, up.get("role")))
    for lg, role in ((h, "host"), (j, "join")):
        if len(lg.all("session_start")) != 1:
            bad.append("I9 %s: %d session_start lines" % (role, len(lg.all("session_start"))))
    sp = [lg.first("session_pace") for lg in (h, j)]
    if None in sp or sp[0].get("fps") != sp[1].get("fps"):
        bad.append("I9 session_pace differs or missing: %s / %s" % tuple(sp))
    for lg, role, code, rev in ((h, "host", args.host_code, args.host_rev),
                                (j, "join", args.join_code, args.join_rev)):
        rid_ = lg.first("rom_id")
        if not rid_ or rid_.get("code") != code or rid_.get("rev") != rev:
            bad.append("I10 %s: rom_id %s, expected %s rev %s" % (role, rid_, code, rev))
    for lg, role, master in ((h, "host", True), (j, "join", False)):
        bt = [v for v in lg.all("ap_val") if v.get("name") == "btype"]
        if not bt:
            if any(m.get("text") == "battle_started" for m in lg.all("ap_mark")):
                bad.append("I11 %s: battle type never logged" % role)
            continue
        v = int(bt[0]["val"], 16)
        if (v & 3) != 3 or bool(v & 4) != master:
            bad.append("I11 %s: gBattleTypeFlags=0x%x" % (role, v))
    return bad


def positions(log):
    """Link-room position probe: name -> (x, y, facing) of gObjectEvents[1]
    (leader) and [2] (member) as THIS console sees them, from the fixture's
    p1_spot/p2_spot (at on_spot) and p1_late/p2_late (600 frames later);
    live coords, i.e. map tile + 7."""
    out = {}
    for v in log.all("ap_val"):
        n = v.get("name", "")
        if n in ("p1_spot", "p2_spot", "p1_late", "p2_late") and "hex" in v:
            b = bytes.fromhex(v["hex"])
            if len(b) >= 9:
                x, y = struct.unpack_from("<hh", b, 0)
                out[n] = (x, y, b[8] & 0x0F)
    return out


SLOT_ADDR = {"leader": "02036e6c", "member": "02036e90"}


def start_skew(log, role):
    """LINK-ROOM START SKEW, from watch_ram (gObjectEvents[1]/[2] x|y) and the
    fixture's walk_start mark, all on THIS console's own clock:
      own_echo_ms   -- from this console starting its walk to its OWN avatar's
                       first step on its own screen (the link round trip a
                       player feels: in the link room even your own step
                       comes back through the link);
      other_first_ms -- when the OTHER player's avatar first stepped on this
                       screen, relative to the same zero.
    The owner saw older builds give the host a ~half-tile head start; the
    host's own_echo_ms against the join's says whether that is still so."""
    marks = {m.get("text"): int(m.get("t_ms", "0")) for m in log.all("ap_mark")
             if m.get("text") in ("in_colosseum", "walk_start")}
    if "walk_start" not in marks or "in_colosseum" not in marks:
        return None
    t0, tc = marks["walk_start"], marks["in_colosseum"]
    own = SLOT_ADDR["leader" if role == "host" else "member"]
    other = SLOT_ADDR["member" if role == "host" else "leader"]
    base, first = {}, {}
    for v in log.all("watch"):
        a, t = v.get("a"), int(v.get("t_ms", "0"))
        if a not in (own, other):
            continue
        if t <= tc:
            base[a] = v.get("v")            # the spawn tile (set before in_colosseum)
        elif a not in first and a in base and v.get("v") != base[a]:
            first[a] = t - t0
    return {"own_echo_ms": first.get(own), "other_first_ms": first.get(other)}


def f2m_sum(H, J):
    """The four FIGHT->move-menu waits of one turn (host L/R, join L/R)
    summed, in ms.  H0 run 3 (arm B): join L stepped +24 frames at turn 18
    while host R fell 13 -- the wait moved between measurements when the two
    scripts' phase shifted, with both queues at <= 3.  A per-measure slope
    reads that as growth; the sum does not."""
    per = {}
    for r in (H, J):
        for b in ("L", "R"):
            for t, ms, _fr in r["f2m"][b]:
                per.setdefault(t, []).append(ms)
    series = [(t, sum(v)) for t, v in sorted(per.items()) if len(v) == 4]
    if not series:
        return None
    ms = [v for _, v in series]
    tail = [x for x in series if x[0] >= 3]
    return {"n": len(series), "first3_median_ms": median(ms[:3]),
            "last3_median_ms": median(ms[-3:]),
            "slope_ms_per_turn": slope([x[0] for x in tail], [x[1] for x in tail])}


def desync(h, j):
    """-> ([(turn, what)], turns compared).  A turn counts as compared only if
    BOTH consoles decoded all FOUR mons and the personality sets agree; any
    less is not evidence of agreement (no conclusion from absence)."""
    fh, _ = fingerprints(h)
    fj, _ = fingerprints(j)
    out, compared = [], 0
    for t in sorted(set(fh) & set(fj)):
        a, b = fh[t], fj[t]
        if len(a) != 4 or len(b) != 4:
            continue
        if set(a) != set(b):
            out.append((t, "different mons: host %s join %s" % (sorted(a), sorted(b))))
            continue
        compared += 1
        for p in sorted(a):
            if a[p] != b[p]:
                out.append((t, "mon %s: host %s join %s" % (p, a[p], b[p])))
    return out, compared


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("host_log")
    ap.add_argument("join_log")
    ap.add_argument("--host-ini"), ap.add_argument("--join-ini")
    ap.add_argument("--host-fixture"), ap.add_argument("--join-fixture")
    ap.add_argument("--expect-crc")
    ap.add_argument("--host-model"), ap.add_argument("--join-model")
    ap.add_argument("--host-code", default="BPRE")
    ap.add_argument("--join-code", default="BPGE")
    ap.add_argument("--host-rev", default="01")
    ap.add_argument("--join-rev", default="01")
    ap.add_argument("--turns", type=int, default=30)
    ap.add_argument("--band", type=float, default=2.0,
                    help="F2M slope noise band, ms/turn (from arm-B null runs)")
    ap.add_argument("--json")
    args = ap.parse_args(argv)

    h, j = Log(args.host_log), Log(args.join_log)
    H, J = console(h, "host", args), console(j, "join", args)
    bad = invariants(h, j, H, J, args)
    ds, compared = desync(h, j)
    res = {"host": H, "join": J, "invalid": bad, "desync": ds,
           "fingerprints_compared": compared, "f2m_sum": f2m_sum(H, J),
           "start_skew": {"host": start_skew(h, "host"), "join": start_skew(j, "join")}}
    if bad:
        res["verdict"] = "INVALID"
    elif ds:
        res["verdict"] = "FAIL: DESYNC"
    else:
        reasons = []
        for r in (H, J):
            if r["turns_completed"] < args.turns:
                reasons.append("%s %s after %d turns" % (r["role"], r["failure"], r["turns_completed"]))
            pd = r["link"]["rfu_pacedrop.n.total"]
            if pd == "unknown":
                reasons.append("%s pacedrop UNKNOWN (no census line)" % r["role"])
            elif pd != 0:
                reasons.append("%s discarded link data (pacedrop=%s)" % (r["role"], pd))
            qd = r["link"]["rfu_qdrop.count"]
            if qd:
                reasons.append("%s receive queue overflowed (rfu_qdrop x%d)" % (r["role"], qd))
            for b in ("f2m_L", "f2m_R"):
                s = r[b]
                if not s:
                    continue
                if s["slope_ms_per_turn"] is not None and s["slope_ms_per_turn"] > args.band:
                    reasons.append("%s %s slope %.1f ms/turn > band %.1f"
                                   % (r["role"], b, s["slope_ms_per_turn"], args.band))
                if s["last3_median_ms"] - s["first3_median_ms"] > 50:
                    reasons.append("%s %s grew %d ms (last3-first3)"
                                   % (r["role"], b, s["last3_median_ms"] - s["first3_median_ms"]))
        need = min(H["turns_completed"], J["turns_completed"]) + 1
        if compared < need:
            reasons.append("fingerprints compared at %d of %d turn boundaries "
                           "(desync status UNKNOWN for the rest)" % (compared, need))
        res["verdict"] = "PASS" if not reasons else "FAIL: " + "; ".join(reasons)

    # A validation fault run (rfu_fault_corrupt) must never read as a clean
    # PASS: either the detector saw the corruption, or the run says in its
    # verdict that it did not (the first in-battle fault run did exactly that).
    armed = [int(v.get("n", "0")) for lg in (h, j) for v in lg.all("rfu_fault_corrupt")]
    fired = sum(len(lg.all("rfu_fault")) for lg in (h, j))
    res["fault"] = {"armed": any(armed), "corrupted": fired}
    # Each console's view of both link players once movement has drained.
    # A disagreement is a link-room movement desync; agreement off the spot
    # is overshoot.  Reported, not scored: the snapshots are taken at each
    # console's own on_spot + 600 frames, not at one shared instant.
    hp, jp = positions(h), positions(j)
    res["positions"] = {"host": hp, "join": jp,
                        "late_disagree": [k for k in ("p1_late", "p2_late")
                                          if k in hp and k in jp and hp[k][:2] != jp[k][:2]]}
    # A SETUP failure with a player OFF HIS SPOT on both consoles alike is the
    # rig's walk, not the link: the autopilot stops holding the d-pad when its
    # own coordinate reads the target, and under a long link round trip the
    # key stream already holds more steps.  Never count that against an arm.
    spots = {"p1_late": (10, 12), "p2_late": (17, 12)}
    off = [k for k in spots if k in hp and k in jp and hp[k][:2] == jp[k][:2]
           and hp[k][:2] != spots[k]]
    res["positions"]["off_spot"] = off
    discarded = any(r["link"]["rfu_pacedrop.n.total"] not in (0, "unknown") or
                    r["link"]["rfu_qdrop.count"] for r in (H, J))
    if off and not discarded and res["verdict"].startswith("FAIL") and "SETUP" in res["verdict"]:
        res["verdict"] = "INVALID"
        bad.append("I13 player off his spot on both consoles (%s): the rig's walk "
                   "overshot, not a link failure" % ", ".join(off))
    elif res["positions"]["late_disagree"] and res["verdict"] != "INVALID":
        res["verdict"] = "FAIL: DESYNC (link-room positions disagree: %s)" % (
            ", ".join(res["positions"]["late_disagree"]))
    # Trace-ring loss can only HIDE failure evidence (a pacedrop or qdrop that
    # never reached the log), never create it: it voids a PASS, and on a
    # failure that other lines prove it is reported, not disqualifying.
    lost = [role for lg, role in ((h, "host"), (j, "join")) if lg.has("rfu_trace_lost")]
    res["trace_incomplete"] = lost
    if lost and res["verdict"] == "PASS":
        res["verdict"] = "INVALID"
        bad.append("I6 %s: rfu trace ring lost events (a PASS cannot rule out "
                   "a hidden drop)" % "/".join(lost))
    if any(armed) and not bad:
        if not fired:
            res["verdict"] = "INVALID"
            bad.append("fault armed (rfu_fault_corrupt) but no packet was corrupted")
        elif res["verdict"] == "PASS":
            res["verdict"] = ("FAULT UNDETECTED: %d packet(s) corrupted, "
                              "fingerprints agree at every boundary" % fired)

    # ---- report --------------------------------------------------------
    print("run: %s  %s" % (args.host_log, args.join_log))
    print("VERDICT: %s" % res["verdict"])
    for b in bad:
        print("  INVALID  %s" % b)
    for t, d in ds[:10]:
        print("  DESYNC   turn %d: %s" % (t, d))
    print("fingerprints compared at %d turn boundaries" % compared)
    if res["fault"]["armed"]:
        print("fault: %d packet(s) corrupted" % res["fault"]["corrupted"])
    if res["positions"]["host"] or res["positions"]["join"]:
        for k in ("p1_spot", "p2_spot", "p1_late", "p2_late"):
            print("link-room %s: host %s join %s" % (k, res["positions"]["host"].get(k),
                                                    res["positions"]["join"].get(k)))
        if res["positions"]["late_disagree"]:
            print("link-room POSITIONS DISAGREE (%s): movement desync"
                  % ", ".join(res["positions"]["late_disagree"]))
    fps = H.get("session_fps") or J.get("session_fps") or 57.0
    print("(each latency: +/-1 emulated frame per stamp, ~%.1f ms at %.2f fps)"
          % (1000.0 / fps, fps))
    fs = res["f2m_sum"]
    if fs:
        print("f2m_sum (4 waits/turn) first3=%s last3=%s slope=%s ms/turn (n=%d)"
              % (fs["first3_median_ms"], fs["last3_median_ms"],
                 "unknown" if fs["slope_ms_per_turn"] is None else "%.1f" % fs["slope_ms_per_turn"],
                 fs["n"]))
    sk = res["start_skew"]
    if sk["host"] or sk["join"]:
        print("start skew (ms from own walk start): host own=%s member-seen=%s | "
              "join own=%s leader-seen=%s"
              % ((sk["host"] or {}).get("own_echo_ms"), (sk["host"] or {}).get("other_first_ms"),
                 (sk["join"] or {}).get("own_echo_ms"), (sk["join"] or {}).get("other_first_ms")))
    if res["trace_incomplete"]:
        print("telemetry: rfu trace ring lost events on %s (failure evidence above "
              "is from lines that were not lost)" % "/".join(res["trace_incomplete"]))
    for r in (H, J):
        print("-- %s: turns_completed=%d failure=%s" % (r["role"], r["turns_completed"], r["failure"]))
        for b in ("f2m_L", "f2m_R", "turn"):
            s = r[b]
            if not s:
                print("   %-6s unknown (no complete turn)" % b)
                continue
            sl = s["slope_ms_per_turn"]
            print("   %-6s first3=%s last3=%s max=%d@t%d slope=%s ms/turn (n=%d)"
                  % (b, s["first3_median_ms"], s["last3_median_ms"], s["max_ms"],
                     s["max_turn"], "unknown" if sl is None else "%.2f" % sl, s["n"]))
        print("   link " + " ".join("%s=%s" % kv_ for kv_ in r["link"].items()))
        if r["fail_line"]:
            print("   ap_fail %s" % r["fail_line"])
        if r["fail_probe"]:
            print("   fail_probe %s" % r["fail_probe"])
    if args.json:
        json.dump(res, open(args.json, "w"), indent=1, default=str)
    return 0 if res["verdict"] == "PASS" else (2 if res["verdict"] == "INVALID" else 1)


if __name__ == "__main__":
    sys.exit(main())

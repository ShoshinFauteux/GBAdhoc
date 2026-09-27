#!/usr/bin/env python3
"""G3: the scorer against synthetic logs, including deliberately broken ones.

A clean pair must PASS; each corruption must come out with the RIGHT verdict
and reason -- INVALID for broken measurement, FAIL: DESYNC for a state
mismatch, FAIL for growing lag.  A scorer that passes a broken log is worse
than no scorer (docs/RIG-DOUBLE-BATTLE.md §2.8)."""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import summarize_battle as sb  # noqa: E402


import struct  # noqa: E402

MONS = {"host": [(0x1000, 0xAAAA0001, 1), (0x1001, 0xAAAA0001, 16)],
        "join": [(0x2002, 0xBBBB0002, 4), (0x2003, 0xBBBB0002, 16)]}


def party_lines(role, it, itv, f, fp_break):
    """The 4 party copies as this console logs them: own = m*, peer's = e*.
    PP of GROWL drops by one per turn on every mon (both consoles agree)."""
    own = MONS[role]
    peer = MONS["join" if role == "host" else "host"]
    out = []
    for tag, (pers, otid, species) in [("m0", own[0]), ("m1", own[1]),
                                       ("e0", peer[0]), ("e1", peer[1])]:
        order = sb.ORDERS[pers % 24]
        dec = bytearray(48)
        struct.pack_into("<H", dec, order.index("G") * 12, species)
        a = order.index("A") * 12
        struct.pack_into("<4H", dec, a, 45, 0, 0, 0)
        pp = 40 - (it - 1)
        if fp_break and role == "join" and it >= fp_break and pers == 0x1001:
            pp -= 1                            # the join sees one more GROWL
        dec[a + 8] = pp
        key = pers ^ otid
        enc = struct.pack("<12I", *[w ^ key for w in struct.unpack("<12I", bytes(dec))])
        out.append("EVT ap_val name=k%s hex=%s it=%d f=%d"
                   % (tag, struct.pack("<II", pers, otid).hex(), itv, f))
        out.append("EVT ap_val name=d%sa hex=%s it=%d f=%d" % (tag, enc[:24].hex(), itv, f))
        out.append("EVT ap_val name=d%sb hex=%s it=%d f=%d" % (tag, enc[24:].hex(), itv, f))
        out.append("EVT ap_val name=h%s val=0x%08x it=%d f=%d" % (tag, 20, itv, f))
    return out


def make(role, turns=30, f2m=lambda t: 60, fp_break=None, drop=None, rid="7-1",
         unknown_key=False, no_exit=False, no_evt_drop=False, dup_key=False,
         fault=None, extra=()):
    L = ["EVT boot_ok", "EVT psp_model code=%d name=%s" % ((2, "PSP-3000") if role == "host" else (0, "PSP-1000")),
         "EVT rig run_id=%s arm=B" % rid, "EVT build eboot_crc=deadbeef size=10",
         "EVT rom_id code=%s rev=01 size=16777216 hdr_crc=0" % ("BPRE" if role == "host" else "BPGE"),
         "EVT rfu_shed_keep n=2", "EVT cfg src=harness key=rfu_shed_keep raw=2"]
    if dup_key:
        L.append("EVT cfg_duplicate src=harness key=net_latency_ms raw=50 ignored=1 (first line wins)")
    if unknown_key:
        L.append("EVT cfg_unknown src=harness key=rfu_shed_kep raw=2")
    if fault:
        L.append("EVT rfu_fault_corrupt n=%d" % (1500 if role == "join" else 0))
    if fault == "fired" and role == "join":
        L.append("EVT rfu_fault corrupted=1500 off=5 net=up")
    L += ["EVT cfg_audit harness_keys=2 config_keys=0",
          "EVT ap_loaded steps=100 file=x crc=0000abcd",
          "EVT net_up role=%s proto=\"gpSP\"" % role,
          "EVT session_start id=%d peers=1" % (0 if role == "host" else 1),
          "EVT session_pace fps=57.00 reason=session_start",
          "EVT ap_mark text=battle_started f=100 t_ms=1000 it=0",
          "EVT ap_val name=btype val=0x%08x it=0 f=100" % (0x2f if role == "host" else 0x2b)]
    f, t = 200, 2000
    for it in range(1, turns + 2):
        post = it == turns + 1
        itv = 0 if post else it
        L.append("EVT ap_mark text=t1 f=%d t_ms=%d it=%d" % (f, t, itv))
        L += party_lines(role, it, itv, f, fp_break)
        if drop and role == "host" and it == drop:
            L.append("EVT evt_gap dropped=3 total=3")
        if post:
            break
        d = f2m(it)
        for name, df in (("t2_fight", 4), ("t3_movemenu", 4 + d), ("t4_move", 9 + d),
                         ("t5", 80 + d), ("t6_fight", 84 + d), ("t7_movemenu", 84 + 2 * d),
                         ("t8_move", 89 + 2 * d)):
            L.append("EVT ap_mark text=%s f=%d t_ms=%d it=%d" % (name, f + df, t + df * 17, it))
        f += 1200 + 2 * d
        t += (1200 + 2 * d) * 17
    L.append("EVT rfu_pacedrop n=0 hi=0 net=up")
    if turns >= 30:
        L.append("EVT ap_mark text=turns_done f=%d t_ms=%d it=0" % (f + 1, t + 17))
    L += [x % {"role": role} if "%(role)" in x else x for x in extra
          if not x.startswith(("host:", "join:")) or x.startswith(role + ":")]
    L = [x.split(":", 1)[1] if x.startswith(("host:", "join:")) else x for x in L]
    if not no_evt_drop:
        L.append("EVT evt_drop total=%d" % (3 if drop and role == "host" else 0))
    if not no_exit:
        L.append("EVT exit code=0")
    return "\n".join(L) + "\n"


def run(td, name, **kw):
    hk = {k: v for k, v in kw.items()}
    hp, jp = os.path.join(td, name + "-h.log"), os.path.join(td, name + "-j.log")
    open(hp, "w").write(make("host", **hk))
    jk = dict(hk)
    if name == "runid":
        jk["rid"] = "8-1"
    open(jp, "w").write(make("join", **jk))
    rc = sb.main([hp, jp, "--host-model", "PSP-3000", "--join-model", "PSP-1000",
                  "--json", os.path.join(td, name + ".json")])
    import json
    return rc, json.load(open(os.path.join(td, name + ".json")))


def main():
    with tempfile.TemporaryDirectory() as td:
        pp = os.path.join(td, "pos.log")
        open(pp, "w").write("EVT ap_val name=p2_late hex=11000c0010000c0004000000 it=0 f=1" + chr(10))
        assert sb.positions(sb.Log(pp)) == {"p2_late": (17, 12, 4)}

        rc, r = run(td, "clean")
        assert r["verdict"] == "PASS", r["verdict"]
        assert r["host"]["turns_completed"] == 30 and r["fingerprints_compared"] == 31

        rc, r = run(td, "gap", drop=12)
        assert r["verdict"] == "INVALID" and any("I6" in x for x in r["invalid"]), r["invalid"]

        rc, r = run(td, "runid")
        assert r["verdict"] == "INVALID" and any("I2" in x for x in r["invalid"]), r["invalid"]

        rc, r = run(td, "unk", unknown_key=True)
        assert r["verdict"] == "INVALID" and any("I4" in x for x in r["invalid"]), r["invalid"]

        rc, r = run(td, "dupkey", dup_key=True)
        assert r["verdict"] == "INVALID" and any("duplicate" in x for x in r["invalid"]), r["invalid"]

        rc, r = run(td, "noexit", no_exit=True)
        assert r["verdict"] == "INVALID" and any("I1" in x for x in r["invalid"]), r["invalid"]

        rc, r = run(td, "nodrop", no_evt_drop=True)
        assert r["verdict"] == "INVALID" and any("evt_drop total" in x for x in r["invalid"])

        rc, r = run(td, "desync", fp_break=17)
        assert r["verdict"] == "FAIL: DESYNC" and r["desync"][0][0] == 17, r["desync"]

        rc, r = run(td, "fault_seen", fault="fired", fp_break=17)
        assert r["verdict"] == "FAIL: DESYNC" and r["fault"]["corrupted"] == 1, r

        rc, r = run(td, "fault_missed", fault="fired")
        assert r["verdict"].startswith("FAULT UNDETECTED") and rc == 1, r["verdict"]

        rc, r = run(td, "fault_idle", fault="armed")
        assert r["verdict"] == "INVALID" and any("no packet" in x for x in r["invalid"]), r

        # I12 RADIO is AIR loss: the join resent 30 % and the host had NONE
        # of those already -> the air lost them -> INVALID ...
        bad_air = ["join:EVT net_stats tx=4200 retx=1260 dup=0 srtt_us=20000",
                   "host:EVT net_stats tx=4200 retx=40 dup=0 srtt_us=20000"]
        rc, r = run(td, "radio", extra=bad_air)
        assert r["verdict"] == "INVALID" and any("I12 join->host" in x for x in r["invalid"]), r["invalid"]
        # ... but the same 30 % resent with the host already holding them is
        # our own RTO under injected latency (spurious), not the air.
        spurious = ["join:EVT net_stats tx=4200 retx=1260 dup=0 srtt_us=120000",
                    "host:EVT net_stats tx=4200 retx=40 dup=1200 srtt_us=120000"]
        rc, r = run(td, "spurious", extra=spurious)
        assert r["verdict"] == "PASS", (r["verdict"], r["invalid"])

        # start skew: own avatar steps 120 ms after walk_start, the other
        # player 300 ms after, on this console's clock
        sp = os.path.join(td, "skew.log")
        open(sp, "w").write("".join(x + chr(10) for x in [
            "EVT watch a=02036e6c v=f000d f=10 t_ms=500",
            "EVT watch a=02036e90 v=f000e f=10 t_ms=500",
            "EVT watch a=02036e6c v=f000d f=150 t_ms=3100",
            "EVT ap_mark text=in_colosseum f=190 t_ms=3800 it=0",
            "EVT ap_mark text=walk_start f=200 t_ms=4000 it=0",
            "EVT watch a=02036e6c v=e000d f=207 t_ms=4120",
            "EVT watch a=02036e90 v=e000e f=218 t_ms=4300"]))
        assert sb.start_skew(sb.Log(sp), "host") == {"own_echo_ms": 120, "other_first_ms": 300}
        assert sb.start_skew(sb.Log(sp), "join") == {"own_echo_ms": 300, "other_first_ms": 120}

        # I13: the colosseum walk failing is the rig's walk -- unless link data
        # was discarded, which makes it a real failure
        walk = ["join:EVT ap_mark text=in_colosseum f=50 t_ms=900 it=0",
                "join:EVT ap_fail step=53 line=122 op=holdram frame=60 it=0 val=0x0000000f"]
        rc, r = run(td, "walk", extra=walk)
        assert r["verdict"] == "INVALID" and any("I13 join" in x for x in r["invalid"]), r["invalid"]
        settle = ["join:EVT ap_mark text=in_colosseum f=50 t_ms=900 it=0",
                  "join:EVT ap_mark text=walk_settle f=55 t_ms=950 it=0",
                  "join:EVT ap_fail step=58 line=127 op=waitram frame=56 it=0 val=0x00000012"]
        rc, r = run(td, "settle", extra=settle)
        assert any("I13 join" in x for x in r["invalid"]), r["invalid"]
        rc, r = run(td, "walk_drop", extra=walk + ["join:EVT rfu_pacedrop n=13 hi=1 net=up"])
        assert not any("I13" in x for x in r["invalid"]), r["invalid"]

        # trace-ring loss voids a PASS ...
        rc, r = run(td, "lost_pass", extra=["host:EVT rfu_trace_lost n=32"])
        assert r["verdict"] == "INVALID" and any("I6" in x for x in r["invalid"]), r
        # ... but a failure proven by other lines stays a FAIL, annotated
        rc, r = run(td, "lost_fail", turns=12,
                    extra=["host:EVT rfu_trace_lost n=32", "host:EVT rfu_qdrop side=host_rx slot=0"])
        assert r["verdict"].startswith("FAIL") and "rfu_qdrop x1" in r["verdict"], r["verdict"]
        assert r["trace_incomplete"] == ["host"]

        # the summed F2M does not read a redistribution as growth
        rc, r = run(td, "sum", f2m=lambda t: 60)
        assert r["f2m_sum"]["slope_ms_per_turn"] is not None and abs(r["f2m_sum"]["slope_ms_per_turn"]) < 1e-6

        rc, r = run(td, "grow", f2m=lambda t: 60 + 4 * t)
        assert r["verdict"].startswith("FAIL") and "slope" in r["verdict"], r["verdict"]

        rc, r = run(td, "short", turns=12)
        assert r["verdict"].startswith("FAIL") and "LAG_TIMEOUT after 12 turns" in r["verdict"], r["verdict"]
    print("summarize_battle: clean PASS; gap/run_id/unknown-key/duplicate-key/truncation/"
          "no-drop-count INVALID; desync and growth FAIL; a fired fault is DESYNC or "
          "FAULT UNDETECTED, never PASS; radio INVALID; trace loss voids only a PASS -- "
          "all as specified")
    return 0


if __name__ == "__main__":
    sys.exit(main())

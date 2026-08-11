#!/usr/bin/env python3
"""summarize_log.py -- turn a 40 KB frontend.log into ~30 lines worth reading.

WHY THIS EXISTS
---------------
Field logs run 35-45 KB each and a session produces two of them.  Pasting
those in full to reason about is mostly noise: the signal in a run is a few
dozen numbers, and burying them in a thousand heartbeat lines makes it harder,
not easier, to hold the thread across a long debugging session.

The rule this encodes: report EXTREMES AND CHANGES, never averages of
averages.  Every real finding in this project came from a maximum, an
asymmetry between the two consoles, or a counter that was zero and stopped
being zero.  Means hid all of them.

Deliberately kept:
  * config, role, build identity                 -- which experiment was this
  * fps: worst window, and whether rendered==emu -- the user-visible number
  * core_phase: mean AND max per phase           -- the max is what clamps us
  * blit_prof                                    -- our own cost
  * transport finals: srtt/rto/retx/txq_hi/spill/dup
  * every RFU counter that is supposed to be 0   -- cmderr, qdrop
  * high-water marks: qhi, rxburst, rxhold
  * rfu_state transitions, compressed            -- how the session ended
  * rfu_flight worst_gap_ms + the final line     -- the run-up to a failure
  * sram_flush worst                             -- the save stall
  * anything unrecognised, verbatim              -- so new events are never
                                                    silently dropped

Usage:  summarize_log.py <log> [<log> ...]
"""
import re
import sys
import os


def nums(pattern, text, group=1):
    return [m.group(group) for m in re.finditer(pattern, text)]


def minmax(vals):
    if not vals:
        return None
    v = [int(x) for x in vals]
    return min(v), max(v), sum(v) // len(v)


def net_final(text):
    """The last EVT net_stats WITH A PEER, as a dict of every k=v on the line.

    Parsed generically instead of by fixed pattern so a field added to the
    event shows up here without this file being edited -- the previous version
    hard-coded eight fields and would have silently dropped the rest.

    The peers!=0 filter matters: the final net_stats is emitted after teardown
    with peers=0, where srtt/rto read 0.  That looks like a flawless link and
    is the exact opposite of the truth.
    """
    lines = [m.group(1) for m in re.finditer(r"EVT net_stats (.*)", text)]
    withpeer = [l for l in lines if not re.search(r"\bpeers=0\b", l)]
    pool = withpeer or lines
    if not pool:
        return None
    return dict(re.findall(r"(\w+)=(\S+)", pool[-1]))


# The -D values the PSP build ships (psp/Makefile).  Named here so a report of
# "200000" is not silently read as a computed RTO when it is the floor.
RTO_FLOOR_US, RTO_CEIL_US = 200000, 800000


def rto_note(v):
    if v is None:
        return "?"
    n = int(v)
    if n == RTO_FLOOR_US:
        return "%dus (AT FLOOR)" % n
    if n == RTO_CEIL_US:
        return "%dus (AT CEILING - clamped, true RTO is larger)" % n
    return "%dus" % n


def cross(paths):
    """Pair two logs from the SAME session and report what only exists across
    them.

    WHY: `retx` is what THIS console retransmitted; `dup` is what THIS console
    received twice.  They are opposite directions, so the rate that says
    whether a retransmission was WASTED -- peer's dup over our retx -- cannot
    be computed from one log.  A full debugging session went by with that
    number sitting in plain sight across two files, invisible because the tool
    only ever looked at one at a time.  Do not remove this.
    """
    seen = []
    for p in paths:
        try:
            text = open(p, "r", errors="replace").read()
        except OSError:
            continue
        m = re.search(r"EVT net_up role=(\w+)", text)
        st = net_final(text)
        if m and st:
            seen.append((m.group(1), os.path.basename(p), st))
    roles = {r: (n, s) for r, n, s in seen}
    if len(seen) != 2 or len(roles) != 2:
        return []   # not a matched host/join pair; say nothing rather than guess

    out = ["=" * 68, "CROSS-CONSOLE  (only computable from both logs)", "=" * 68]
    for a, b in (("join", "host"), ("host", "join")):
        if a not in roles or b not in roles:
            continue
        retx = int(roles[a][1].get("retx", 0))
        dup = int(roles[b][1].get("dup", 0))
        bw = roles[b][1].get("beyondwin")
        if bw is not None and int(bw) > 0:
            note = ("  <-- peer logged beyondwin=%s; dup is NOT purely "
                    "retransmission" % bw)
        else:
            note = ""
        pct = (dup * 100 // retx) if retx else 0
        out.append("%-5s retx=%-6d -> %-5s dup=%-6d  wasted=%d%%%s"
                   % (a, retx, b, dup, pct, note))
    j, h = roles.get("join"), roles.get("host")
    if j and h:
        out.append("srtt   : join=%sus  host=%sus   (symmetric at 40fps; "
                   "diverges above)" % (j[1].get("srtt_us"), h[1].get("srtt_us")))
        out.append("rto    : join=%s  host=%s"
                   % (rto_note(j[1].get("rto_us")),
                      rto_note(h[1].get("rto_us"))))
    return out


def summarize(path):
    try:
        text = open(path, "r", errors="replace").read()
    except OSError as e:
        return ["!! cannot read %s: %s" % (path, e)]

    out = ["=" * 68, os.path.basename(path), "=" * 68]

    # ---- identity -------------------------------------------------------
    m = re.search(r"LOG config loaded: (.*)", text)
    if m:
        cfg = m.group(1)
        keep = ("net_frameskip", "net_session_fps", "snap", "core_phase",
                "blit_mode", "gu_defer", "rfu_rx_cap", "net_pace_match")
        bits = [kv for kv in cfg.split() if kv.split("=")[0] in keep]
        out.append("config : " + " ".join(bits))
    for pat, label in ((r"EVT net_up role=(\w+)", "role"),
                       (r"EVT blit_mode (req=\d+ mode=\d+ name=\w+)", "blit"),
                       (r"EVT pixfmt=(\S+)", "pixfmt"),
                       (r"EVT sram_load size=\d+ crc=(\w+)", "sav_crc")):
        m = re.search(pat, text)
        if m:
            out.append("%-7s: %s" % (label, m.group(1)))

    # ---- fps: the worst window is the interesting one -------------------
    # Only windows AFTER the pacer finished ramping are the session's rate.
    # The old "< 45" filter treated the ramp window as the worst frame rate and
    # reported 33.08 for a run that then held 40.00 +/- 0.01 -- a clamp miss
    # that never happened, on the same line as real ones.
    ramp = re.search(r"EVT session_pace .*reason=ramp_done", text)
    body = text[ramp.end():] if ramp else text
    fps = re.findall(r"EVT fps emu=([\d.]+) rendered=([\d.]+) skipped=(\d+)", body)
    allfps = re.findall(r"EVT fps emu=([\d.]+)", text)
    if fps:
        worst = min(fps, key=lambda f: float(f[0]))
        best = max(fps, key=lambda f: float(f[0]))
        skipped = sum(int(f[2]) for f in fps)
        out.append("fps    : %s..%s over %d post-ramp windows (%d total) "
                   "| rendered=%s | skipped=%d"
                   % (worst[0], best[0], len(fps), len(allfps), worst[1], skipped))
    elif allfps:
        out.append("fps    : %d windows, NONE post-ramp (run ended during ramp)"
                   % len(allfps))

    # ---- core_phase: max matters more than mean -------------------------
    cp = re.findall(r"EVT core_phase .*?tot=(\d+)/(\d+) cpu=(\d+)/(\d+) "
                    r"vid=(\d+)/(\d+) blt=(\d+)/(\d+)", text)
    if cp:
        tot_mean = sum(int(c[0]) for c in cp) // len(cp)
        tot_max = max(int(c[1]) for c in cp)
        cpu_max = max(int(c[3]) for c in cp)
        vid_max = max(int(c[5]) for c in cp)
        out.append("core   : tot mean=%d max=%d | cpu max=%d | vid max=%d  (us; budget 16743)"
                   % (tot_mean, tot_max, cpu_max, vid_max))

    bp = re.findall(r"EVT blit_prof .*?stage=(\d+)/\d+ gu=(\d+)/\d+ "
                    r"wait=(\d+)/\d+ tot=(\d+)", text)
    if bp:
        last = bp[-1]
        out.append("blit   : stage=%s gu=%s wait=%s tot=%s" % last)

    # ---- transport: final counters --------------------------------------
    # Take the last sample WITH A PEER STILL CONNECTED.  The final net_stats
    # is emitted after teardown with peers=0, where srtt/rto read 0 -- which
    # looks like a flawless link and is the opposite of the truth.
    st = net_final(text)
    if st:
        out.append("net    : retx=%s dup=%s srtt=%sus rto=%sus retx_pct=%s%% "
                   "txq_hi=%s spill=%s"
                   % (st["retx"], st["dup"], st["srtt_us"], st["rto_us"],
                      st["retx_pct"], st["txq_hi"], st["spill"]))
        # Fields added by ADR-0048; absent from older logs, so print only what
        # is actually there rather than reporting 0 for "not measured".
        extra = [(k, st[k]) for k in ("rttvar_us", "beyondwin", "retx_age",
                                      "reorder_hi") if k in st]
        if extra:
            out.append("net+   : " + " ".join("%s=%s" % kv for kv in extra))

    # ---- counters that are SUPPOSED to be zero --------------------------
    for ev, label in (("rfu_cmderr", "cmderr"), ("rfu_qdrop", "qdrop")):
        n = len(re.findall(r"EVT %s" % ev, text))
        flag = "" if n == 0 else "   <-- NONZERO"
        out.append("%-7s: %d%s" % (label, n, flag))

    # ---- high-water marks ----------------------------------------------
    for pat, label in ((r"EVT rfu_qhi side=(\w+) depth=(\d+)", "qhi"),
                       (r"EVT rfu_rxburst frame_max=(\d+)", "rxburst"),
                       (r"EVT rfu_rxhold frame_max=(\d+)", "rxhold")):
        hits = re.findall(pat, text)
        if hits:
            if label == "qhi":
                per = {}
                for side, d in hits:
                    per[side] = max(per.get(side, 0), int(d))
                out.append("qhi    : " + " ".join("%s=%d" % kv for kv in per.items()))
            else:
                out.append("%-7s: max=%d" % (label, max(int(h) for h in hits)))

    # ---- how the session ended ------------------------------------------
    st = re.findall(r"EVT rfu_state new=(\w+) cause=(\d+)", text)
    if st:
        # collapse runs of identical transitions; the interesting part is the
        # order of DISTINCT states, especially whether a disconnect (7/8)
        # appeared before the reset (9).
        seq, prev = [], None
        for s in st:
            if s != prev:
                seq.append("%s/%s" % s)
                prev = s
        out.append("rfu_st : " + " -> ".join(seq[-8:]))
    for pat in (r"EVT rfu_link_down reason=(\d+)", r"EVT exit code=(\d+)"):
        m = re.search(pat, text)
        if m:
            out.append("%-7s: %s" % (pat.split()[1].split("=")[0], m.group(1)))

    # ---- flight recorder -------------------------------------------------
    m = re.search(r"EVT rfu_flight end worst_gap_ms=(\d+) at_frame=(\d+)", text)
    if m:
        out.append("flight : worst_gap=%sms at f=%s  (healthy client ~33ms)"
                   % (m.group(1), m.group(2)))
    fl = re.findall(r"EVT rfu_fl (f=\d+ .*)", text)
    if fl:
        out.append("flight-: " + fl[-1][:110])

    # ---- save stall ------------------------------------------------------
    sf = re.findall(r"EVT sram_flush .*?ms=([\d.]+)", text)
    if sf:
        out.append("sram   : %d flushes, worst=%sms"
                   % (len(sf), max(sf, key=float)))
    pm = re.findall(r"session_pace_miss actual=([\d.]+) fixed=([\d.]+)", text)
    if pm:
        out.append("pacemis: %d misses, worst actual=%s vs target %s"
                   % (len(pm), min(pm, key=lambda p: float(p[0]))[0], pm[0][1]))

    # ---- anything we do not recognise, so new events never vanish --------
    known = ("heartbeat", "fps ", "core_prof", "core_phase", "blit_prof",
             "net_stats", "adhoc_stats", "sess_cost", "smc_addr", "smc_block",
             "smc_code", "rfu_state", "rfu_login", "rfu_cmd", "rfu_rxburst",
             "rfu_qhi", "rfu_fl", "rfu_flight", "input mask", "net_up",
             "net_down", "session_pace", "sram_flush", "blit_mode", "pixfmt",
             "ff ", "ui_", "rom_loaded", "bios=", "av_info", "sram_load",
             "audio_rate", "mem_free", "log_thread", "boot_ok", "clock=",
             "rfu_rx_cap", "gu_defer", "config_saved", "sav_backup",
             "adhoc_up", "skip_policy", "net_pace_match", "net_tx_thread",
             "session_start", "peer_connected", "peer_disconnected",
             "session_stop", "exit code", "rfu_link_down", "rfu_qdrop",
             "rfu_cmderr", "rfu_unkcmd", "rfu_rxhold", "skip_release",
             "smc_", "session_pace_miss")
    unknown = {}
    for line in re.findall(r"EVT (\S+)", text):
        k2 = [k.strip() for k in known]
        if not any(line.startswith(k) or k.startswith(line) for k in k2):
            unknown[line] = unknown.get(line, 0) + 1
    if unknown:
        out.append("NEW    : " + " ".join("%s(%d)" % kv for kv in unknown.items()))

    return out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    for p in sys.argv[1:]:
        print("\n".join(summarize(p)))
        print()
    if len(sys.argv) == 3:
        block = cross(sys.argv[1:])
        if block:
            print("\n".join(block))
            print()
    return 0


if __name__ == "__main__":
    sys.exit(main())

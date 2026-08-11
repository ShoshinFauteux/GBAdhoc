#!/usr/bin/env python3
"""ab_compare.py -- is the difference between two builds real, or is it noise?

WHY THIS EXISTS
---------------
Every wrong conclusion in this project so far came from comparing single runs.
A 1.73x CPU asymmetry that was 1.15x once every window was counted.  A
retransmission count that halved between two runs for reasons that were never
established.  A layout change credited with an improvement it could not
physically have produced.  In each case the number was real and the COMPARISON
was not, because one sample has no spread and a difference without a spread is
not a measurement.

So this tool never reports a bare delta.  It reports the delta against the
observed run-to-run spread, and refuses to call anything a difference unless it
clears that spread.  When it cannot tell, it says so -- "no difference detected"
and "not enough runs to tell" are different answers and are printed differently.

Usage:
    ab_compare.py --a "SPLIT-join-*.log" --b "RFUX-join-*.log"
    ab_compare.py --a "runs/base/*join*.log" --b "runs/cand/*join*.log" --metric cpu_mean

With one run per arm it will still print the numbers, clearly marked as
indicative only, plus how many runs each arm would need for the observed gap to
become decidable.
"""
import argparse
import glob
import math
import re
import statistics as st
import sys


# Each metric: how to pull every sample of it from one log.  Window-level
# metrics give many samples per run, which is what makes a single run
# interpretable at all -- but see the note in summarise() about why the spread
# WITHIN a run is not the spread we need.
METRICS = {
    "cpu_mean":    (r"EVT core_phase .*?cpu=(\d+)/\d+",            "us"),
    "cpu_max":     (r"EVT core_phase .*?cpu=\d+/(\d+)",            "us"),
    "vid_mean":    (r"EVT core_phase .*?vid=(\d+)/\d+",            "us"),
    "tot_mean":    (r"EVT core_phase .*?tot=(\d+)/\d+",            "us"),
    "tot_max":     (r"EVT core_phase .*?tot=\d+/(\d+)",            "us"),
    "blit_tot":    (r"EVT blit_prof .*?tot=(\d+)",                 "us"),
    "retx":        (r"EVT net_stats .*?retx=(\d+)",                "pkts"),
    "srtt_us":     (r"EVT net_stats .*?srtt_us=(\d+)",             "us"),
    "txq_hi":      (r"EVT net_stats .*?txq_hi=(\d+)",              "slots"),
    "reorder_hi":  (r"EVT net_stats .*?reorder_hi=(\d+)",          "slots"),
    "rfux_calls":  (r"EVT core_phase .*?rfux=(\d+)/\d+",           "calls/f"),
    "rfux_us":     (r"EVT core_phase .*?rfux=\d+/(\d+)",           "us/f"),
    "fps":         (r"EVT fps emu=([\d.]+)",                       "fps"),
}


def samples(path, pattern):
    try:
        body = open(path, "r", errors="replace").read()
    except OSError:
        return []
    return [float(m) for m in re.findall(pattern, body)]


def run_value(path, pattern):
    """One number per RUN. Deliberately the mean of that run's windows: the
    unit of comparison is a run, because that is the unit that gets randomised
    by everything we cannot control (thermals, radio, where the player stood)."""
    s = samples(path, pattern)
    return st.mean(s) if s else None


def welch(a, b):
    """Welch's t and approximate two-sided p. Unequal variances assumed --
    the arms are different builds, so equal variance is not a safe default."""
    na, nb = len(a), len(b)
    if na < 2 or nb < 2:
        return None, None
    va, vb = st.variance(a), st.variance(b)
    se = math.sqrt(va / na + vb / nb)
    if se == 0:
        return None, None
    t = (st.mean(b) - st.mean(a)) / se
    df = (va / na + vb / nb) ** 2 / (
        (va / na) ** 2 / (na - 1) + (vb / nb) ** 2 / (nb - 1))
    # Normal approximation to the t tail; fine at the effect sizes we care
    # about and avoids a scipy dependency on a machine that may not have one.
    p = math.erfc(abs(t) / math.sqrt(2))
    return t, p


def runs_needed(a, b):
    """Roughly how many runs PER ARM would make the observed gap decidable at
    p~0.05, 80% power. Answers 'how many runs do you actually need' instead of
    leaving it to taste."""
    if len(a) < 2 and len(b) < 2:
        return None
    pool = [x for x in (a + b)]
    if len(pool) < 2:
        return None
    sd = st.stdev(pool)
    d = abs(st.mean(b) - st.mean(a))
    if d == 0 or sd == 0:
        return None
    return max(2, int(math.ceil(2 * (2.8 * sd / d) ** 2)))


CFG_KEYS = ("net_session_fps", "core_phase", "gu_defer", "blit_mode",
            "rfu_rx_cap", "net_frameskip", "snap")


def config_of(path):
    """The parameters this run actually used, straight from its own log.

    This is what makes 'group by config' possible without bookkeeping: a run
    carries its own arm label.  Filenames lie -- configs do not."""
    try:
        body = open(path, "r", errors="replace").read()
    except OSError:
        return None
    m = re.search(r"LOG config loaded: (.*)", body)
    if not m:
        return None
    kv = dict(p.split("=", 1) for p in m.group(1).split() if "=" in p)

    # `LOG config loaded:` IS A PRE-OVERRIDE SNAPSHOT, NOT THE EFFECTIVE CONFIG.
    #
    # main_psp.c applies .gpsp-harness.ini AFTER printing that line, so any key
    # the harness channel sets is wrong here -- the log shows gu_defer=0 on runs
    # whose authoritative `EVT gu_defer=1` says otherwise.  Since the harness ini
    # is exactly how experiments are driven, every arm bucketed as IDENTICAL and
    # this tool would have reported "1 distinct config" across a night of A/Bs.
    #
    # So override the snapshot with the post-override events wherever one
    # exists.  These are emitted after the ini is applied and are what the run
    # actually ran with.
    for key, pat in (("gu_defer",        r"EVT gu_defer=(\d+)"),
                     ("rfu_rx_cap",      r"EVT rfu_rx_cap n=(\d+)"),
                     ("core_phase",      r"EVT core_phase lvl=(\d+)")):
        mm = re.search(pat, body)
        if mm:
            kv[key] = mm.group(1)

    # The APPLIED pacing rate, not the requested one: pace_snap_x100 admits only
    # 59.73/29.97/19.98, so 40.00 and 50.00 are the same arm and must not look
    # like two.  Prefer the snap line, fall back to net_pace_match's `fixed=`.
    mm = (re.search(r"session_pace_snap req=[\d.]+ applied=([\d.]+)", body)
          or re.search(r"EVT net_pace_match .*?fixed=([\d.]+)", body))
    if mm:
        kv["net_session_fps"] = mm.group(1)

    cfg = " ".join("%s=%s" % (k, kv[k]) for k in CFG_KEYS if k in kv)

    # ADR-0054: the BUILD is part of the arm.  Without it, three runs from three
    # different binaries that shared a config grouped as one arm -- which is
    # how a build change gets credited to a config change, or hidden by one.
    #
    # The stamp is __DATE__"_"__TIME__ and __DATE__ CONTAINS SPACES ("Aug  6
    # 2026_05:57:32"), so \S+ captured just "Aug" and every build on earth
    # bucketed together -- the precise failure this line exists to prevent.
    b = re.search(r"EVT build stamp=(.+?)\s*$", body, re.M)
    stamp = re.sub(r"\s+", " ", b.group(1)) if b else None
    return ("build=%s " % stamp if stamp else "build=UNSTAMPED ") + cfg


def survey(paths):
    """Bucket runs by config and report how many of each.

    100 runs with 100 different parameter sets support no conclusion at all.
    This prints the brutal version of that: how many DISTINCT arms exist and
    how few runs each one actually has."""
    buckets = {}
    for f in paths:
        c = config_of(f)
        buckets.setdefault(c or "(no config line)", []).append(f)
    print("%-4s %s" % ("runs", "config"))
    print("-" * 100)
    usable = 0
    for cfg, fs in sorted(buckets.items(), key=lambda kv: -len(kv[1])):
        flag = "" if len(fs) >= 3 else ("   <-- too few to conclude anything"
                                        if len(fs) < 2 else "   <-- barely")
        if len(fs) >= 3:
            usable += 1
        print("%-4d %s%s" % (len(fs), cfg, flag))
    print()
    print("%d distinct configs, %d run(s) total, %d config(s) with >=3 runs."
          % (len(buckets), len(paths), usable))
    if usable == 0:
        print("Nothing here supports a conclusion. Repeat a config before changing it.")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", help="glob for baseline logs")
    ap.add_argument("--b", help="glob for candidate logs")
    ap.add_argument("--metric", action="append",
                    help="restrict to these metrics (default: all)")
    ap.add_argument("--alpha", type=float, default=0.05)
    ap.add_argument("--survey", help="glob: bucket these logs by their own "
                                     "recorded config and show run counts")
    args = ap.parse_args()

    if args.survey:
        fs = sorted(glob.glob(args.survey))
        if not fs:
            print("no logs matched")
            return 2
        return survey(fs)

    if not args.a or not args.b:
        print("need --a and --b (or --survey)")
        return 2
    fa, fb = sorted(glob.glob(args.a)), sorted(glob.glob(args.b))
    if not fa or not fb:
        print("no logs matched (A=%d, B=%d)" % (len(fa), len(fb)))
        return 2

    print("A: %d run(s)  %s" % (len(fa), ", ".join(f.split("\\")[-1] for f in fa)))
    print("B: %d run(s)  %s" % (len(fb), ", ".join(f.split("\\")[-1] for f in fb)))
    single = len(fa) < 2 or len(fb) < 2
    if single:
        print("\n*** ONE RUN PER ARM: deltas below are INDICATIVE ONLY. ***")
        print("*** No spread exists, so none of them can be called real. ***")
    print()

    names = args.metric or list(METRICS)
    print("%-12s %10s %10s %9s   %s" % ("metric", "A", "B", "delta", "verdict"))
    print("-" * 76)
    for name in names:
        if name not in METRICS:
            continue
        pat, unit = METRICS[name]
        va = [v for v in (run_value(f, pat) for f in fa) if v is not None]
        vb = [v for v in (run_value(f, pat) for f in fb) if v is not None]
        if not va or not vb:
            continue
        ma, mb = st.mean(va), st.mean(vb)
        d = mb - ma
        pct = (100.0 * d / ma) if ma else 0.0

        if single:
            verdict = "indicative only"
        else:
            _t, p = welch(va, vb)
            if p is None:
                verdict = "no spread"
            elif p < args.alpha:
                verdict = "REAL (p=%.3f)" % p
            else:
                n = runs_needed(va, vb)
                verdict = ("not detected (p=%.2f" % p) + (
                    ", need ~%d/arm)" % n if n else ")")
        print("%-12s %10.1f %10.1f %+8.1f%%   %s"
              % (name, ma, mb, pct, verdict))

    if single:
        print("\nTo decide any of these, run each arm at least 3 times.")
        print("ab_compare will then print p-values and, where it still cannot")
        print("tell, how many runs per arm the observed gap would require.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

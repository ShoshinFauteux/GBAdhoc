#!/usr/bin/env python3
"""bw_report.py CSV LOG [--title T] [--json OUT]

Per-scene thin-client bandwidth from lb_host --bw output.  Scenes are the
autopilot `evt` marks in LOG (EVT ap_mark text=NAME f=N); a scene runs from its
mark to the next.  For each scene and frame-skip k the cost of a sent frame
is the codec's bytes; KB/s = mean(bytes per sent frame) * 60/k / 1000.

Codecs (see lb_host.c): raw = ME-granularity pages (1 KiB VRAM pages, whole
OAM/palette, changed 128-byte capture lines, 20-byte seed); rle = zero-run
XOR patch; rlz4 = rle then LZ4; defl1 = deflate level 1 of the XOR delta.
Audio: DirectSound FIFO bytes (8-bit PCM as the GBA plays it) per frame.
"""
import argparse, csv, json, statistics as st

KS = [1, 2, 3, 4, 6]
CODECS = ["raw", "rle", "rlz4", "defl1"]
COL = {"raw": "raw", "rle": "rle", "rlz4": "rlz4", "defl1": "defl1"}
BUDGETS = [100, 200, 300]

def pct(v, p):
    v = sorted(v)
    if not v: return 0
    i = min(len(v) - 1, int(round(p / 100 * (len(v) - 1))))
    return v[i]

def marks(log):
    out = []
    for ln in open(log, errors="replace"):
        if "ap_mark" in ln:
            name = ln.split("text=")[1].split()[0]
            f = int(ln.split(" f=")[1].split()[0])
            out.append((f, name))
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv"); ap.add_argument("log")
    ap.add_argument("--title", default="")
    ap.add_argument("--json")
    ap.add_argument("--skip-first", type=int, default=2,
                    help="frames after a state load to drop (keyframe)")
    o = ap.parse_args()
    rows = list(csv.DictReader(open(o.csv)))
    ms = marks(o.log)
    # drop frames just after each state (re)load: they are full keyframes
    seg_first = {}
    for r in rows:
        seg_first.setdefault(r["seg"], int(r["f"]))
    def scene_of(f):
        name = None
        for mf, mn in ms:
            if f > mf: name = mn
        return name
    scenes = {}
    order = []
    keyf = []
    for r in rows:
        f = int(r["f"])
        if f - seg_first[r["seg"]] < o.skip_first:
            if r["k1_rlz4"]: keyf.append(int(r["k1_rlz4"]))
            continue
        sc = scene_of(f)
        if sc is None or sc == "end": continue
        if sc not in scenes:
            scenes[sc] = []; order.append(sc)
        scenes[sc].append(r)
    res = {"title": o.title, "scenes": {}, "keyframe_rlz4": keyf}
    print(f"# {o.title}")
    print(f"{'scene':20s} {'fr':>5s} | k=1 bytes/frame rlz4: med  p95   max | KB/s@60 raw  rle  rlz4 defl1 | "
          f"KB/s rlz4 k=2 k=3 k=4 k=6 | audio fifo B/f | fits 100/200/300 (k, KB/s)")
    for sc in order + ["ALL"]:
        rs = [r for s in order for r in scenes[s]] if sc == "ALL" else scenes[sc]
        d = {"frames": len(rs)}
        for k in KS:
            for c in CODECS:
                v = [int(r[f"k{k}_{COL[c]}"]) for r in rs if r[f"k{k}_{COL[c]}"] != ""]
                if not v: continue
                d[f"k{k}_{c}"] = {"med": st.median(v), "p95": pct(v, 95), "max": max(v),
                                  "mean": st.mean(v), "kbps": st.mean(v) * 60 / k / 1000,
                                  "p95kbps": pct(v, 95) * 60 / k / 1000}
        au = [int(r["fifoA"]) + int(r["fifoB"]) for r in rs]
        d["audio_fifo_bpf"] = st.mean(au) if au else 0
        d["vchg_med"] = st.median([int(r["k1_vchg"]) for r in rs])
        d["vchg_max"] = max(int(r["k1_vchg"]) for r in rs)
        fits = {}
        for b in BUDGETS:
            fits[b] = None
            for k in KS:
                if d[f"k{k}_rlz4"]["p95kbps"] <= b:
                    fits[b] = (k, round(d[f"k{k}_rlz4"]["p95kbps"], 1)); break
        d["fits_p95"] = fits
        # adaptive partial-update model (lb_host b<B>_*): delivered fps,
        # longest frozen stretch, and queueing delay of a sent frame
        for b in BUDGETS:
            if f"b{b}_sent" not in rs[0]: continue
            sent = [int(r[f"b{b}_sent"]) for r in rs]
            gaps, g = [], 0
            for x in sent:
                if x: gaps.append(g); g = 0
                else: g += 1
            gaps.append(g)
            lat = [float(r[f"b{b}_backlog"]) / (b * 1000) * 1000 for r in rs if r[f"b{b}_sent"] == "1"]
            d[f"b{b}"] = {"fps": 60 * sum(sent) / len(sent),
                          "max_freeze_ms": (max(gaps) + 1) * 1000 / 60,
                          "p95_delay_ms": pct(lat, 95) if lat else 0}
        res["scenes"][sc] = d
        k1 = d["k1_rlz4"]
        print(f"{sc:20s} {len(rs):5d} | {k1['med']:>27.0f} {k1['p95']:5d} {k1['max']:5d} | "
              f"{d['k1_raw']['kbps']:>11.1f} {d['k1_rle']['kbps']:4.1f} {k1['kbps']:5.1f} {d['k1_defl1']['kbps']:5.1f} | "
              f"{d['k2_rlz4']['kbps']:>12.1f} {d['k3_rlz4']['kbps']:4.1f} {d['k4_rlz4']['kbps']:4.1f} {d['k6_rlz4']['kbps']:4.1f} | "
              f"{d['audio_fifo_bpf']:>14.0f} | " + " ".join(
                  f"{b}:{('k'+str(v[0])) if v else 'NO'}" for b, v in fits.items()))
    print("\nadaptive partial updates (send when the link is free; audio ADPCM reserved):")
    print(f"{'scene':20s} " + " | ".join(f"{b:>3d} KB/s: fps  freeze  delay" for b in BUDGETS))
    for sc in order + ["ALL"]:
        d = res["scenes"][sc]
        if "b100" not in d: continue
        print(f"{sc:20s} " + " | ".join(
            f"{d[f'b{b}']['fps']:14.1f} {d[f'b{b}']['max_freeze_ms']:6.0f}ms {d[f'b{b}']['p95_delay_ms']:4.0f}ms"
            for b in BUDGETS))
    if keyf:
        print(f"keyframe (full state, rlz4 from zero): {max(keyf)} bytes")
    if o.json:
        json.dump(res, open(o.json, "w"), indent=1, default=str)

if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Make the XMB's animated icon: psp/assets/gbadhoc_icon1.pmf.

    python tools/logo/make_icon1.py [--frames-only] [--keep DIR]

Needs numpy, Pillow and an ffmpeg with libx264 on PATH.  Nothing else: the
PSMF container is written here (no UMD Stream Composer).

THE PICTURE (the splash design's ICON1 storyboard, docs/design/UI-MOCKUPS.md).  144x80
like ICON0, 90 frames at 29.97 fps (3 s), looping: the 3 px hinted letters
are always there; the five arcs sweep in left to right, innermost first,
0.1 s apart (0.2-1.0 s); the console bar wipes left to right (1.0-1.4 s);
hold; arcs and bar fade out over the last 0.4 s, so the loop closes on the
letters alone, which is how it opens.  Dark, to match the dark ICON0/PIC1.
ICON0 stays the still: the XMB plays ICON1 only while the icon is
highlighted, and an ICON1 the firmware cannot play just leaves ICON0 up.

THE STREAM.  Modelled byte for byte on a Sony-authored 144x80 PSMF (the one
pspautotests/tests/video/*/test.pmf carries, whose expected outputs were
recorded on a real PSP), measured with tools/logo/pmf_inspect.py:

  * H.264 Main profile, level 2.1, CABAC, I and P pictures only (POC type 2),
    one reference frame, frame_mbs_only, full-range BT.601, VUI timing for
    29.97 fps with fixed_frame_rate, NAL HRD (2.56 Mbit/s, 2.56 Mbit CPB),
    pic_struct present.  Every access unit is AUD, SEI (buffering period on
    an IDR, picture timing on all), slice; SPS/PPS repeated on each IDR.
    x264's own "x264 - core ..." user-data SEI is removed.
  * MPEG-2 program stream in 2048-byte packs, mux rate 25000 (10 Mbit/s).
    Each GOP starts a pack with a system header, a private_stream_2 (0xBF)
    table of the GOP's access-unit sizes, and a video PES (0xE0) with PTS,
    DTS (= PTS - 3003) and a P-STD buffer extension; every other pack is one
    video PES filling it; a GOP's last pack ends in a padding stream (0xBE).
    PTS/DTS also ride on the first PES that starts an access unit at least
    15 frames after the last stamped one, as Sony's muxer does.
  * The 2048-byte "PSMF0014" header with the reference's fields, the
    timestamps (first PTS 90000, end = last PTS + 3003) and the data size.

Re-run after changing the logo (tools/logo/logomark.py) or the timing.
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

import numpy as np

import logomark as M

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, 'psp', 'assets', 'gbadhoc_icon1.pmf')

W, H = 144, 80
FPS_NUM, FPS_DEN = 30000, 1001
N_FRAMES = 90
FRAME_TICKS = 3003               # 90 kHz ticks per frame at 29.97
FIRST_PTS = 90000                # the reference's: one second
GOP = 45                         # two GOPs; the reference's run 20-59 frames
BG = (0, 0, 0)                   # the dark theme page, as ICON0
INK = M.DARK_INK

PACK = 2048
MUX_RATE = 25000                 # units of 50 bytes/s
SCR_STEP = 44237                 # 27 MHz ticks per pack at MUX_RATE
PTS_EVERY = 15                   # frames between stamped PES (Sony: ~16)


# ---------------------------------------------------------------- frames --
def icon_frame(t, bg=BG, ink=INK, dur=3.0):
    """The splash design's icon_frame (docs/design/UI-MOCKUPS.md), t in seconds."""
    L, _ = M.render_letters(stroke_px=3, hint=True)
    lh, lw = L.shape
    rb_h, bar_h = 20, 2
    total = rb_h + lh + 3 + bar_h
    oy = (H - total) // 2 + rb_h
    ox = (W - lw) // 2
    cx = ox + M.RB_CX * (3 / M.STROKE)
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    dist = np.sqrt((xx + 0.5 - cx) ** 2 + (yy + 0.5 - oy) ** 2)
    ang = np.degrees(np.arctan2(-(yy + 0.5 - oy), xx + 0.5 - cx))
    half = np.clip(oy - (yy + 0.5) + 0.5, 0, 1)
    fade = 1.0 if t < 2.6 else max(0.0, 1 - (t - 2.6) / 0.4)
    layers = []
    radii = [18, 15, 12, 9, 6]
    for i, (col, r) in enumerate(zip(M.RB_COL, radii)):
        k = 4 - i                                  # inner arc first
        p = np.clip((t - 0.2 - 0.1 * k) / 0.4, 0, 1)
        sweep = 180 - 180 * p                      # arc exists for ang >= sweep
        amask = np.clip((ang - sweep) / 2.0 + 0.5, 0, 1)
        cov = np.clip(2 / 2 + 0.5 - np.abs(dist - r), 0, 1) * half * amask * fade
        layers.append((M.hexrgb(col), cov.astype(np.float32)))
    letters = np.zeros((H, W), np.float32)
    letters[oy:oy + lh, ox:ox + lw] = L
    layers.append((ink, letters))
    bp = np.clip((t - 1.0) / 0.4, 0, 1)
    for col, cov in M.bar_layers(3 / M.STROKE, ox, oy, (H, W), y=lh + 3,
                                 h=bar_h, width=lw):
        wipe = np.clip((ox + lw * bp - xx), 0, 1)
        layers.append((col, (cov * wipe * fade).astype(np.float32)))
    bgarr = np.zeros((H, W, 3), np.float32) + np.array(bg, np.float32)
    return M.to_image(M.composite(bgarr, layers))


def render_frames(d):
    for i in range(N_FRAMES):
        icon_frame(i * FPS_DEN / FPS_NUM).save(os.path.join(d, 'f%03d.png' % i))


# ---------------------------------------------------------------- encode --
def encode(d):
    out = os.path.join(d, 'icon1.264')
    params = ':'.join([
        'bframes=0', 'ref=1', 'keyint=%d' % GOP, 'min-keyint=%d' % GOP,
        'scenecut=0', 'weightp=0', 'force-cfr=1', 'aud=1', 'nal-hrd=vbr', 'pic-struct=1',
        'vbv-maxrate=2560', 'vbv-bufsize=2560', 'crf=14', 'repeat-headers=1',
        'fullrange=on', 'colorprim=bt470bg', 'transfer=bt470bg',
        'colormatrix=bt470bg'])
    cmd = ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y',
           '-framerate', '%d/%d' % (FPS_NUM, FPS_DEN),
           '-i', os.path.join(d, 'f%03d.png'),
           '-vf', 'scale=out_color_matrix=bt601:out_range=full',
           '-pix_fmt', 'yuvj420p', '-c:v', 'libx264',
           '-profile:v', 'main', '-level:v', '2.1', '-x264-params', params,
           '-f', 'h264', out]
    subprocess.run(cmd, check=True)
    return open(out, 'rb').read()


# ------------------------------------------------------------ elementary --
def nal_units(es):
    """[(start, end)] of every NAL including its 4-byte start code."""
    starts = []
    i = 0
    while True:
        j = es.find(b'\x00\x00\x01', i)
        if j < 0:
            break
        s = j - 1 if j > 0 and es[j - 1] == 0 else j
        starts.append(s)
        i = j + 3
    return [(s, starts[k + 1] if k + 1 < len(starts) else len(es))
            for k, s in enumerate(starts)]


def sei_is_user_data(nal):
    """SEI whose first message is user_data_unregistered (x264's banner)."""
    p = nal.find(b'\x00\x00\x01') + 4
    t = 0
    while nal[p] == 0xFF:
        t += 255
        p += 1
    return t + nal[p] == 5


def access_units(es):
    """Split at AUDs, normalise every start code to 4 bytes, drop the x264
    banner SEI.  Returns [(bytes, is_idr)]."""
    aus, cur, idr = [], [], False
    for s, e in nal_units(es):
        nal = es[s:e]
        body = nal[nal.find(b'\x00\x00\x01') + 3:]
        typ = body[0] & 0x1F
        if typ == 9 and cur:
            aus.append((b''.join(cur), idr))
            cur, idr = [], False
        if typ == 6 and sei_is_user_data(nal):
            continue
        if typ == 5:
            idr = True
        cur.append(b'\x00\x00\x00\x01' + body)
    if cur:
        aus.append((b''.join(cur), idr))
    return aus


# ------------------------------------------------------------- program ----
def pts_bytes(prefix, v):
    return bytes([(prefix << 4) | (((v >> 30) & 7) << 1) | 1,
                  (v >> 22) & 0xFF, (((v >> 15) & 0x7F) << 1) | 1,
                  (v >> 7) & 0xFF, ((v & 0x7F) << 1) | 1])


def pack_header(scr27):
    base, ext = scr27 // 300, scr27 % 300
    b = bytearray(b'\x00\x00\x01\xba')
    b += bytes([0x40 | (((base >> 30) & 7) << 3) | 0x04 | ((base >> 28) & 3),
                (base >> 20) & 0xFF,
                (((base >> 15) & 0x1F) << 3) | 0x04 | ((base >> 13) & 3),
                (base >> 5) & 0xFF,
                ((base & 0x1F) << 3) | 0x04 | ((ext >> 7) & 3),
                ((ext & 0x7F) << 1) | 1])
    b += bytes([(MUX_RATE >> 14) & 0xFF, (MUX_RATE >> 6) & 0xFF,
                ((MUX_RATE & 0x3F) << 2) | 3, 0xF8])
    return bytes(b)


# The reference's system header, verbatim: rate bound 25000, one audio
# bound, video stream 0xE0 (P-STD 85 KiB) and private_stream_1 (8 KiB).
SYSTEM_HEADER = bytes.fromhex('000001bb000c80c35180f07fb9e055bde008')
PES_EXT = bytes([0x1E, 0x60, 0x55])          # P-STD buffer, scale 1, 85 KiB


def au_table(sizes, end_packs):
    """private_stream_2 (0xBF): the GOP's access-unit sizes, and the pack
    (relative to the GOP's first) in which each of the first four ends."""
    idx = (list(end_packs[:4]) + [0, 0, 0, 0])[:4]
    body = bytearray(b'\x01\xe0')
    for v in idx:
        body += struct.pack('>H', v)
    body += b'\x00\x00\x00\x00'
    body += struct.pack('>HH', 4 * len(sizes) + 2, len(sizes))
    for s in sizes:
        body += struct.pack('>HH', 0x0080, s)
    return b'\x00\x00\x01\xbf' + struct.pack('>H', len(body)) + bytes(body)


def layout_gop(aus, first_frame, bf_len, stamped_from):
    """Plan one GOP's packs.  Returns a list of packs, each a list of
    (es_bytes, pts_frame_or_None), plus the pack index each AU ends in."""
    es = b''.join(a for a, _ in aus)
    au_start = []
    o = 0
    for a, _ in aus:
        au_start.append(o)
        o += len(a)
    packs, ends = [], [None] * len(aus)
    pos, k, last_stamp = 0, 0, stamped_from
    while pos < len(es):
        room = PACK - 14 - 6 - 3
        if k == 0:
            room -= len(SYSTEM_HEADER) + bf_len
        # The first AU that starts in this pack decides the stamp.
        stamp = None
        n0 = next((n for n, s in enumerate(au_start) if s >= pos), None)
        if n0 is not None and au_start[n0] < pos + room:
            f = first_frame + n0
            if k == 0 or f - last_stamp >= PTS_EVERY:
                stamp = f
        if stamp is not None:
            room -= 10 + len(PES_EXT)
            # The stamp only stands if its AU still starts in the room left.
            if au_start[n0] >= pos + room:
                room += 10 + len(PES_EXT)
                stamp = None
        if stamp is not None:
            last_stamp = stamp
        take = min(room, len(es) - pos)
        for n, s in enumerate(au_start):
            e = s + len(aus[n][0])
            if pos < e <= pos + take:
                ends[n] = k
        packs.append((es[pos:pos + take], stamp))
        pos += take
        k += 1
    return packs, ends, last_stamp


def build_program(aus):
    gops, cur = [], []
    for i, (a, idr) in enumerate(aus):
        if idr and cur:
            gops.append(cur)
            cur = []
        cur.append((a, idr))
    gops.append(cur)
    out = bytearray()
    scr = int((FIRST_PTS - FRAME_TICKS) * 300 - 0.85 * 27000000)
    scr = max(scr, 0)
    frame, last_stamp = 0, -10 ** 9
    for g in gops:
        sizes = [len(a) for a, _ in g]
        bf_len = len(au_table(sizes, [0] * 4))
        packs, ends, last_stamp = layout_gop(g, frame, bf_len, last_stamp)
        bf = au_table(sizes, ends)
        assert len(bf) == bf_len
        for k, (chunk, stamp) in enumerate(packs):
            p = bytearray(pack_header(scr))
            if k == 0:
                p += SYSTEM_HEADER + bf
            if stamp is not None:
                pts = FIRST_PTS + stamp * FRAME_TICKS
                hdr = (bytes([0x81, 0xC1, 13]) + pts_bytes(3, pts) +
                       pts_bytes(1, pts - FRAME_TICKS) + PES_EXT)
            else:
                hdr = bytes([0x81, 0x00, 0])
            left = PACK - len(p) - 6 - len(hdr) - len(chunk)
            if 0 < left < 6:
                # No room for a padding packet: stuff the PES header.
                hdr = hdr[:2] + bytes([hdr[2] + left]) + hdr[3:] + b'\xff' * left
                left = 0
            pes = hdr + chunk
            p += b'\x00\x00\x01\xe0' + struct.pack('>H', len(pes)) + pes
            if left >= 6:
                p += (b'\x00\x00\x01\xbe' + struct.pack('>H', left - 6) +
                      b'\xff' * (left - 6))
            assert len(p) == PACK, len(p)
            out += p
            scr += SCR_STEP
        frame += len(g)
    return bytes(out)


def ts6(v):
    return struct.pack('>HI', 0, v)


def psmf_header(data_len, n_frames):
    end = FIRST_PTS + n_frames * FRAME_TICKS
    h = bytearray(0x800)
    h[0:8] = b'PSMF0014'
    struct.pack_into('>II', h, 8, 0x800, data_len)
    struct.pack_into('>I', h, 0x50, 0x3E)
    h[0x54:0x5A] = ts6(FIRST_PTS)
    h[0x5A:0x60] = ts6(end)
    struct.pack_into('>II', h, 0x60, MUX_RATE, 90000)
    h[0x68:0x6A] = b'\x01\x01'
    struct.pack_into('>I', h, 0x6A, 0x24)
    h[0x6E:0x74] = ts6(FIRST_PTS)
    h[0x74:0x7A] = ts6(end)
    h[0x7A:0x7C] = b'\x00\x01'
    struct.pack_into('>IH', h, 0x7C, 0x12, 1)
    # Stream 0: video 0xE0, P-STD as the system header, no EP map, 144x80.
    h[0x82:0x92] = bytes([0xE0, 0x00, 0x20, 0x55, 0, 0, 0, 0, 0, 0, 0, 0,
                          W // 16, H // 16, 0, 0])
    return bytes(h)


def main(argv):
    keep = None
    if '--keep' in argv:
        keep = argv[argv.index('--keep') + 1]
        os.makedirs(keep, exist_ok=True)
    d = keep or tempfile.mkdtemp(prefix='icon1-')
    render_frames(d)
    if '--frames-only' in argv:
        print('frames in %s' % d)
        return 0
    aus = access_units(encode(d))
    assert len(aus) == N_FRAMES, len(aus)
    prog = build_program(aus)
    with open(OUT, 'wb') as f:
        f.write(psmf_header(len(prog), len(aus)))
        f.write(prog)
    print('wrote %s  (%d bytes, %d frames, %d packs)'
          % (os.path.relpath(OUT, ROOT), os.path.getsize(OUT), len(aus),
             len(prog) // PACK))
    if not keep:
        shutil.rmtree(d, ignore_errors=True)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

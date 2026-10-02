#!/usr/bin/env python3
"""compare.py RUNS OUTDIR -- every PPSSPP GE dump of the overlay against the
model (docs/UI-OVERLAY.md).

RUNS holds run_ppsspp.sh output: demo-dark, demo-light (ui_demo) and
ctl-dark, ctl-light (ui_controls_demo), all with the mockups' backdrop
injected.  For each dump this renders the model of the state the walker left
the screen in, and reports:

  exact   pixels that differ at all
  >1LSB   pixels that differ by more than one RGB565 step in any channel
          (8 in R/B, 4 in G at 8 bits) -- a coordinate, colour, alpha, glyph
          or ordering error shows up here; blend rounding cannot
  max     largest channel difference

Thumbnail rectangles (the slots screen's 46x30 previews, GE bilinear vs
PIL's resampler) are excluded from the counts and reported on their own.
Where the screen is the design verbatim, the designer's PNG is compared too.
Writes OUTDIR/<run>-<dump>.png side-by-sides and OUTDIR/results.json."""
import os, sys, json
import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import overlay_model as O                  # noqa: E402
from pspdraw import G_TRI, G_O, G_X, G_SQ  # noqa: E402

FRAME = O.P.ASSETS['frame']
MOCK_PNG = os.path.join(O.MOCK, 'A-overlay')


def load_thumb(path):
    raw = open(path, 'rb').read()[16:16 + 64 * 42 * 2]
    v = np.frombuffer(raw, '<u2').reshape(42, 64).astype(np.int64)
    r, g, b = v & 31, (v >> 5) & 63, v >> 11
    return Image.fromarray(np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4),
                                     (b << 3) | (b >> 2)], -1).astype(np.uint8))


def scenes(run, theme):
    """(dump name, model fn, design PNG or None, masks) for one run."""
    t = 'light' if theme == 'light' else 'dark'
    rd = os.path.join(RUNS, run)
    stem = os.path.join(rd, 'roms', 'Pokemon - Emerald Version')
    out = []
    if run.startswith('demo'):
        for k in range(1, 8):
            out.append(('ge_ui_open_%d' % k, lambda s, k=k: O.menu(s, FRAME, k=k), None, []))
        out.append(('ge_ui_0', lambda s: O.menu(s, FRAME), 'menu-%s.png' % t, []))
        fx_thumbs = [O.P.ASSETS['thumbs'][i] for i in range(5)]
        masks = [(20, O.band_y(92 + i * 30, 32) + 1, 46, 30) for i in range(5)]
        out.append(('ge_ui_1', lambda s: O.slots(s, FRAME, 'save', 3, [2, 2, 2, 0, 2], fx_thumbs),
                    'slots-save-%s.png' % t, masks))
        th = list(fx_thumbs)
        th[1] = load_thumb(stem + '.st1.thumb')      # the demo saved slot 2
        out.append(('ge_ui_2', lambda s: O.slots(s, FRAME, 'load', 3, [2, 2, 2, 0, 2], th),
                    'slots-load-%s.png' % t, masks))
        rows = O.settings_rows(scale='stretch', theme=t)
        out.append(('ge_ui_3', lambda s: O.settings(s, FRAME, rows, 4), None, []))
        out.append(('ge_ui_4', lambda s: O.wireless(s, FRAME), 'wireless-%s.png' % t, []))
    else:
        rows = O.settings_rows(scale='fit', theme=t)
        out.append(('ge_ctl_0', lambda s: O.settings(s, FRAME, rows, len(rows) - 1), None, []))
        C = O.controls
        none_v = {'Video preset': []}
        ss = dict(none_v, Screenshot=['L', 'R'])
        steal = dict(ss, A=[G_SQ], **{'Fast-forward': []})
        out += [
            ('ge_ctl_1', lambda s: C(s, FRAME, cursor=('A', 0)), None, []),
            ('ge_ctl_2', lambda s: C(s, FRAME, none_v, ('Video preset', 1)), None, []),
            ('ge_ctl_3', lambda s: C(s, FRAME, none_v, ('Screenshot', 1), capture='Screenshot',
                                     timer=TIMERS[0]), None, []),
            ('ge_ctl_4', lambda s: C(s, FRAME, ss, ('Screenshot', 1)), None, []),
            ('ge_ctl_5', lambda s: C(s, FRAME, steal, ('A', 0),
                                     note=G_SQ + ' was Fast-forward  -  Fast-forward is now blank (disabled)'),
             None, []),
            ('ge_ctl_6', lambda s: C(s, FRAME, steal, ('Reset to defaults', 1),
                                     note='Press ' + G_X + ' again to reset every control'), None, []),
            ('ge_ctl_7', lambda s: C(s, FRAME, None, ('Reset to defaults', 1),
                                     note='Every control is back to its default'), None, []),
            ('ge_ctl_8', lambda s: C(s, FRAME, None, ('Fast-forward', 1), capture='Fast-forward',
                                     timer=TIMERS[1]), 'controls-capture-%s.png' % t, []),
            ('ge_ctl_9', lambda s: C(s, FRAME, {'Fast-forward': [G_TRI], 'Video preset': []},
                                     ('Fast-forward', 1),
                                     note=G_TRI + ' was Video preset  -  Video preset is now blank (disabled)'),
             'controls-conflict-%s.png' % t, []),
            ('ge_ctl_10', lambda s: C(s, FRAME, None, ('B', 0)), 'controls-%s.png' % t, []),
        ]
    return out


def diffstat(a, b, masks):
    d = np.abs(a.astype(int) - b.astype(int))
    sh = np.array([3, 2, 3])                  # 8-bit -> the 565 field
    steps = np.abs((a.astype(int) >> sh) - (b.astype(int) >> sh))
    big = (steps > 1).any(axis=2)
    anyd = (d > 0).any(axis=2)
    keep = np.ones(anyd.shape, bool)
    for x, y, w, h in masks:
        keep[y:y + h, x:x + w] = False
    return dict(exact=int((anyd & keep).sum()), gt1lsb=int((big & keep).sum()),
                max=int(d.max(axis=2)[keep].max()),
                masked_exact=int((anyd & ~keep).sum()),
                masked_gt1lsb=int((big & ~keep).sum()))


def sheet(imgs, labels, path):
    w, h = 480, 272
    S = Image.new('RGB', (w * len(imgs), h + 16), (40, 40, 40))
    dr = ImageDraw.Draw(S)
    for i, (im, lab) in enumerate(zip(imgs, labels)):
        S.paste(im, (i * w, 16))
        dr.text((i * w + 4, 2), lab, fill=(230, 230, 230))
    S.save(path, optimize=True)


def diffimg(a, b):
    d = np.abs(a.astype(int) - b.astype(int)).max(axis=2)
    out = np.zeros(a.shape, np.uint8)
    out[d > 0] = (90, 90, 0)                  # within blend rounding
    sh = np.array([3, 2, 3])
    out[(np.abs((a.astype(int) >> sh) - (b.astype(int) >> sh)) > 1).any(axis=2)] = (255, 0, 0)
    return Image.fromarray(out)


if __name__ == '__main__':
    RUNS, OUTD = sys.argv[1], sys.argv[2]
    os.makedirs(OUTD, exist_ok=True)
    results = {}
    for run in sorted(os.listdir(RUNS)):
        if not (run.startswith('demo-') or run.startswith('ctl-')):
            continue
        theme = run.split('-')[1]
        log = open(os.path.join(RUNS, run, 'frontend.log'), errors='replace').read()
        TIMERS = [int(l.split('=')[1]) for l in log.splitlines()
                  if l.startswith('EVT ge_dump_capture timer=')] + [0, 0]
        for name, fn, png, masks in scenes(run, theme):
            dump = os.path.join(RUNS, run, name + '.bmp')
            if not os.path.exists(dump):
                results['%s/%s' % (run, name)] = 'MISSING'
                continue
            s = O.surface(theme, None)
            fn(s)
            model = s.finish()
            calls = s.calls
            d = np.asarray(Image.open(dump).convert('RGB'))
            m = np.asarray(model)
            r = dict(model=diffstat(d, m, masks), model_calls=calls)
            imgs, labels = [], []
            if png and os.path.exists(os.path.join(MOCK_PNG, png)):
                mk = Image.open(os.path.join(MOCK_PNG, png)).convert('RGB')
                r['design'] = diffstat(d, np.asarray(mk), masks)
                imgs.append(mk); labels.append('design ' + png)
            imgs += [Image.fromarray(d), model, diffimg(d, m)]
            labels += ['PPSSPP ' + name, 'model', 'diff (red: >1 LSB)']
            sheet(imgs, labels, os.path.join(OUTD, '%s-%s.png' % (run, name)))
            results['%s/%s' % (run, name)] = r
            print('%-24s model exact=%6d >1lsb=%4d max=%3d%s' % (
                run + '/' + name, r['model']['exact'], r['model']['gt1lsb'], r['model']['max'],
                ('   design exact=%6d >1lsb=%5d' % (r['design']['exact'], r['design']['gt1lsb']))
                if 'design' in r else ''))
    json.dump(results, open(os.path.join(OUTD, 'results.json'), 'w'), indent=1)

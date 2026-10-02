#!/usr/bin/env python3
"""overlay_model.py -- Fable's Direction A as IMPLEMENTED, through the design
model (docs/UI-OVERLAY.md).

builds/menu-mockups/mockups.py (class Overlay) is the design: its numbers are
the spec.  This file draws the screens psp/ui_psp.c draws -- the same calls,
in the same order, with the REAL content (the Controls table of ctl_map.c,
the Settings rows of ui_psp.c, the state the harness walkers leave each
screen in) -- through pspdraw's primitives, on a GESurface (the GE's integer
arithmetic).  compare.py then holds every PPSSPP GE dump against it.

Where the implementation follows the design verbatim (menu, wireless, the
slots screen) the design's own PNG is compared too.
"""
import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
MOCK = os.environ.get('UI_MOCKUPS',
                      os.path.normpath(os.path.join(HERE, '..', '..', '..', 'menu-mockups')))
sys.path.insert(0, MOCK)
sys.path.insert(0, HERE)
import pspdraw as P                       # noqa: E402
import mockups as M                       # noqa: E402
from gesurface import GESurface           # noqa: E402
from pspdraw import mix565, G_TRI, G_O, G_X, G_SQ, FE_FONT_H  # noqa: E402

GAME = 'Pokemon - Emerald Version'
ROOM = 'GPSP07'
OPEN_FRAMES = 8


def surface(theme, backdrop):
    return GESurface(theme, 'GBA', backdrop)


# ---- chrome (ui_psp.c ov_*) -------------------------------------------------
def backdrop(s, scrim, frame):
    s.image_px(0, 0, frame)
    if scrim > 0:
        s.rect(0, 0, 480, 272, s.C['bg_bot'], scrim)
    s.gradient_a(0, 152, 480, 120, s.C['bg_bot'], 0, 165)


def head(s, sub=None, title=None, room=ROOM):
    s.logo(20, 18, s.C['title'], 255)
    if title:
        s.text_hd(20 + P.LOGO_W + 14, 16, title, s.C['sel'])
    if sub:
        s.text(22, 48, sub, s.C['dim'])
    M.ROOM = room
    M.header_cluster(s, y=19)


# Optical centring (video_psp.c vid_band_y / vid_text_y_in, docs/UI-OVERLAY.md
# section 10): the CAP HEIGHT of a line drawn at y -- the atlas 'H': rows
# y+yoff .. y+yoff+h-1 -- is centred in the box, the odd spare row below.
_H = P.FACE_UI.g[ord('H') - P.FU_FIRST]
CAP_TOP, CAP_H = _H[5], _H[3]


def band_y(text_y, h):
    return text_y + CAP_TOP - (h - CAP_H) // 2


def text_y_in(box_y, box_h):
    return box_y + (box_h - CAP_H) // 2 - CAP_TOP


def band(s, y, w, h):
    """ui_psp.c ov_band(): y is the row's TEXT y."""
    by = band_y(y, h)
    s.rect(0, by, w, h, s.C['accent'], 40)
    s.rect(0, by, 4, h, s.C['accent'], 255)


def footer(s, hint):
    P.footer(s, hint)


def plate(s, msg):
    s.rect(0, 252, 480, 20, s.C['accent'], 40)
    s.text_center(254, msg, s.C['sel'])


# ---- menu ------------------------------------------------------------------
MENU = ['Resume', 'Save state', 'Load state', 'Wireless', 'Settings',
        'Quit to game list', 'Exit']


def menu(s, frame, k=OPEN_FRAMES, cur=0):
    fade = 256 * (OPEN_FRAMES - k) // OPEN_FRAMES
    dy = 8 * (OPEN_FRAMES - k) // OPEN_FRAMES
    backdrop(s, 170 * k // OPEN_FRAMES, frame)
    head(s, GAME)
    for i, l in enumerate(MENU):
        y = 88 + i * 22 + dy
        c = s.C['sel'] if i == cur else s.C['item']
        if i == cur and k >= OPEN_FRAMES:
            band(s, y, s.text_w(l) + 44, 24)
        s.text(18, y, l, mix565(c, s.C['bg_bot'], fade))
    footer(s, G_X + ' select     ' + G_O + ' resume')


# ---- slots -----------------------------------------------------------------
def slots(s, frame, mode, cur, has, thumbs):
    """has[i]: 0 none, 1 state, 2 state + preview; thumbs[i]: 64x42 PIL."""
    backdrop(s, 170, frame)
    head(s, GAME, 'SAVE STATE' if mode == 'save' else 'LOAD STATE')
    for i in range(5):
        y = 92 + i * 30
        c = i + 1 == cur
        ty = band_y(y, 32) + 1
        if c:
            band(s, y, 330, 32)
        if has[i] == 2:
            s.image(20, ty, 46, 30, thumbs[i], 255)
        else:
            w = 'old' if has[i] else 'empty'
            s.rect(20, ty, 46, 30, s.C['card'], 255)
            s.text(20 + (46 - s.text_w(w)) // 2, text_y_in(ty, 30), w, s.C['dim'])
        s.text(78, y, 'Slot %d' % (i + 1), s.C['sel'] if c else s.C['item'])
        s.text_right(300, y, 'saved' if has[i] else 'empty',
                     s.C['value'] if has[i] else s.C['dim'])
        if c and (mode == 'save' or has[i]):
            s.text(322, y, G_X + ' load' if mode != 'save' else
                   (G_X + ' overwrite' if has[i] else G_X + ' save here'), s.C['dim'])
    footer(s, 'DPAD slot     ' + G_X + ' select     ' + G_O + ' back')


# ---- settings --------------------------------------------------------------
# (header, label, value, enabled) in ui_psp.c set_rows order.
def settings_rows(scale='stretch', filt='nearest', ambient='off', theme='dark',
                  shell='shelf', ff='3x', ffmode='hold', fps='off',
                  ctl='default', osd='shown'):
    return [(1, 'WIRELESS', None, 1), (0, 'Room code', ROOM, 1),
            (0, 'Session overlay', osd, 1),
            (1, 'DISPLAY', None, 1), (0, 'Video scale', scale, 1),
            (0, 'Video filter', filt if scale != '2x' else 'sharp (2x)', scale != '2x'),
            (0, 'Ambient bars', ambient, 1), (0, 'GB palette', 'GB / GBC only', 0),
            (1, 'INTERFACE', None, 1), (0, 'Theme', theme, 1), (0, 'Menu style', shell, 1),
            (1, 'GAMEPLAY', None, 1), (0, 'Fast-forward', ff, 1),
            (0, 'FF button (Square)', ffmode, 1),
            (0, 'FPS counter', fps, 1),
            (1, 'CONTROLS', None, 1), (0, 'Button mapping', ctl, 1)]


TOP, BOT = 76, 244
SET_BAND_W, SET_BAND_H = 460, 20


def set_row_bot(y):
    """ui_psp.c set_row_bot(): the cursor band's lower edge."""
    return max(band_y(y, SET_BAND_H) + SET_BAND_H, y + FE_FONT_H)


def set_row_y(rows, idx):
    y = TOP
    for i in range(idx):
        y += FE_FONT_H + 3 if rows[i][0] else FE_FONT_H
    return y


def set_scroll(rows, cur, scroll=0):
    """ui_psp.c set_scroll_follow()."""
    top = set_row_y(rows, cur)
    bot = set_row_bot(top)
    mx = max(0, set_row_bot(set_row_y(rows, len(rows) - 1)) - BOT)
    if cur > 0 and rows[cur - 1][0]:
        top = set_row_y(rows, cur - 1)
    if top - scroll < TOP:
        scroll = top - TOP
    if bot - scroll > BOT:
        scroll = bot - BOT
    return max(0, min(scroll, mx))


def settings(s, frame, rows, cur, scroll=0, console_tag='GBA'):
    backdrop(s, 200, frame)
    head(s, None, 'SETTINGS')
    s.text_right(460, 48, ROOM, s.C['dim'])
    scroll = set_scroll(rows, cur, scroll)
    s.clip(0, TOP - 2, 480, BOT - TOP + 4)
    for i, (hdr, lab, val, en) in enumerate(rows):
        y = set_row_y(rows, i) - scroll
        if y + FE_FONT_H <= TOP - 2 or y >= BOT + 2:
            continue
        if hdr:
            lx = 20 + s.text_w(lab) + 8
            s.text(20, y, lab, s.C['accent'])
            if lab == 'DISPLAY':
                tag = '(%s)' % console_tag
                tx = 20 + s.text_w(lab) + 6
                s.text(tx, y, tag, s.CON)
                lx = tx + s.text_w(tag) + 8
            s.rect(lx, y + 8, 440 - lx, 1, s.C['accent_dk'], 120)
            continue
        sel = i == cur
        if sel:
            band(s, y, SET_BAND_W, SET_BAND_H)
        s.text(30, y, lab, (s.C['sel'] if sel else s.C['item']) if en else s.C['dim'])
        s.text_right(440, y, val, (s.C['value'] if sel else s.C['accent_dk']) if en else s.C['dim'])
    s.clip_off()
    total = set_row_bot(set_row_y(rows, len(rows) - 1)) - TOP
    view = BOT - TOP
    if total > view:
        th = max(8, view * view // total)
        s.rect(470, TOP, 2, view, s.C['card'], 255)
        s.rect(470, TOP + view * scroll // total, 2, th, s.C['accent_dk'], 255)
    footer(s, 'DPAD move/change   ' + G_X + ' select   ' + G_O + ' back')


# ---- controls --------------------------------------------------------------
# ctl_map.c's table: label, default chord (ctl_button_order spelling: SELECT,
# START, L, R, UP, DOWN, LEFT, RIGHT, TRI, O, X, SQ).
GAME_ROWS = [('A', [G_O]), ('B', [G_X]), ('L', ['L']), ('R', ['R']),
             ('Start', ['START']), ('Select', ['SELECT']), ('Up', ['UP']),
             ('Down', ['DOWN']), ('Left', ['LEFT']), ('Right', ['RIGHT'])]
SHORT_ROWS = [('Fast-forward', [G_SQ]), ('Video preset', [G_TRI]),
              ('Quick save', ['SELECT', 'L']), ('Quick load', ['SELECT', 'R']),
              ('Screenshot', ['SELECT', 'L', 'R']), ('Pause screen', ['SELECT', G_TRI]),
              ('Mystery Gift', ['SELECT', 'DOWN'])]
ORDER = ['SELECT', 'START', 'L', 'R', 'UP', 'DOWN', 'LEFT', 'RIGHT', G_TRI, G_O, G_X, G_SQ]


def chip(s, x, y, label, kind, alpha=255, h=16):
    """ui_psp.c chip(): y is the row's TEXT y; the plate is centred on it."""
    w = 12 + s.text_w(label)
    C = s.C
    ty, y = y, band_y(y, h)
    if kind == 'bound':
        s.rect(x, y, w, h, C['accent'], 28 if s.light else 40)
        s.text(x + 6, ty, label, C['sel'])
    elif kind == 'locked':
        s.rect(x, y, w, h, C['card'], 255)
        s.text(x + 6, ty, label, C['dim'])
    else:
        col, a = (C['dim'], 160) if kind == 'blank' else (s.CON, alpha)
        s.rect(x, y, w, 1, col, a); s.rect(x, y + h - 1, w, 1, col, a)
        s.rect(x, y, 1, h, col, a); s.rect(x + w - 1, y, 1, h, col, a)
        s.text(x + 6, ty, label, col)
    return w


def combo_right(s, xr, y, parts, kind, alpha=255):
    if not parts:
        w = 12 + s.text_w('none')
        chip(s, xr - w, y, 'none', 'blank')
        return w
    w = sum(12 + s.text_w(p) for p in parts) + (len(parts) - 1) * (s.text_w('+') + 8)
    x = xr - w
    for i, p in enumerate(parts):
        if i:
            s.text(x + 4, y, '+', s.C['dim'])
            x += s.text_w('+') + 8
        x += chip(s, x, y, p, kind, alpha)
    return w


def pulse(timer, phase2=False):
    """ui_psp.c ctl_pulse()."""
    if phase2:
        return 255
    t = timer % 48
    tri = t * 256 // 24 if t < 24 else (48 - t) * 256 // 24
    return 255 - ((255 - 96) * tri >> 8)


def controls(s, frame, binds=None, cursor=('A', 0), capture=None, timer=0,
             note=None, footer_kind='list'):
    """binds: {label: parts}; cursor: (label, col); capture: label being
    captured; note: plate text (conflict/refusal) or None."""
    b = {l: p for l, p in GAME_ROWS + SHORT_ROWS}
    b.update(binds or {})
    backdrop(s, 200, frame)
    head(s, None, 'CONTROLS')
    pitch = min(19, (230 - 68) // (10 - 1))
    cols = [(20, 170, 'GAME BUTTONS',
             [(l, 'act') for l, _ in GAME_ROWS]),
            (206, 254, 'SHORTCUTS',
             [(l, 'act') for l, _ in SHORT_ROWS] +
             [('Menu', 'fixed'), ('Home', 'fixed'), ('Reset to defaults', 'reset')])]
    fixed = {'Menu': 'START+SELECT', 'Home': 'HOME'}
    for cx, cw, title, items in cols:
        lx = cx + s.text_w(title) + 8
        s.text(cx, 48, title, s.C['accent'])
        s.rect(lx, 56, cx + cw - 10 - lx, 1, s.C['accent_dk'], 120)
        for i, (lab, kind) in enumerate(items):
            y = 68 + i * pitch
            sel = lab == cursor[0]
            if sel:
                by = band_y(y, pitch + 1)
                s.rect(cx - 8, by, cw + 6, pitch + 1, s.C['accent'], 40)
                s.rect(cx - 8, by, 3, pitch + 1, s.C['accent'], 255)
            s.text(cx + 4, y, lab, s.C['dim'] if kind == 'fixed' else
                   (s.C['sel'] if sel else s.C['item']))
            xr = cx + cw - 10
            if kind == 'fixed':
                w = combo_right(s, xr, y, [fixed[lab]], 'locked')
                s.text_right(xr - w - 8, y, 'fixed', s.C['dim'])
            elif kind == 'act':
                if capture == lab:
                    combo_right(s, xr, y, ['press...'], 'capture', pulse(timer))
                else:
                    combo_right(s, xr, y, b[lab], 'bound')
    if capture:
        game = capture in dict(GAME_ROWS)
        ask = 'Press a button for' if game else 'Press a button or combo for'
        t5 = '%s %s      cancels in 5 s' % (ask, capture)
        secs = (300 - timer + 59) // 60
        s.rect(0, 252, 480, 20, s.C['accent'], 40)
        s.text((480 - s.text_w(t5)) // 2, 254,
               '%s %s      cancels in %d s' % (ask, capture, secs), s.C['sel'])
    elif note:
        plate(s, note)
    elif footer_kind == 'reset':
        footer(s, G_X + ' reset   ' + G_O + ' back      blank = disabled')
    else:
        footer(s, G_X + ' rebind   ' + G_SQ + ' none   ' + G_TRI + ' default   '
               + G_O + ' back      blank = disabled')


# ---- wireless --------------------------------------------------------------
def wireless(s, frame, cur=0):
    backdrop(s, 170, frame)
    head(s, None, 'WIRELESS')
    s.text_right(460, 48, ROOM, s.C['dim'])
    s.text(22, 72, 'Link two PSPs over ad-hoc WiFi.  Both consoles', s.C['dim'])
    s.text(22, 72 + FE_FONT_H + 2, 'must use the same room code.', s.C['dim'])
    rows = [('Host session', None), ('Join: scan for rooms', None),
            ('Join room code', ROOM), ('Mystery Gift', 'phone'), ('Back', None)]
    for i, (lab, val) in enumerate(rows):
        y = 120 + i * 22
        if i == cur:
            band(s, y, 300, 24)
        s.text(18, y, lab, s.C['sel'] if i == cur else s.C['item'])
        if val:
            s.text_right(300, y, val, s.C['accent_dk'])
    footer(s, G_X + ' select   DPAD change code   ' + G_O + ' back')

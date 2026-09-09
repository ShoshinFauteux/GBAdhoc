"""Match a user's ROM folder to libretro-thumbnails names, and fetch covers.

OWNERSHIP.  This tool only ever fetches art for a file that is ALREADY in the
user's ROM folder.  It does not find, link to, or download games; a cover with
no matching local .gba is skipped.  That is the whole ownership check -- it is
not DRM and does not pretend to be, it just means the tool cannot be used as a
catalogue for games someone does not have.
"""
import json
import os
import re
import urllib.parse
import urllib.request

REPO = "libretro-thumbnails/Nintendo_-_Game_Boy_Advance"
RAW = ("https://raw.githubusercontent.com/%s/master/Named_Boxarts/" % REPO)
TREE = ("https://api.github.com/repos/%s/git/trees/master?recursive=1" % REPO)

_CACHE = os.path.join(os.path.dirname(__file__), "names.json")


def thumbnail_index(refresh=False):
    """Every Named_Boxart filename in the repo, cached on disk.

    The contents API truncates at 1000 entries and the set is ~6000, which is
    how an earlier attempt silently 404'd on half the library.  The git tree
    API returns all of it in one request.
    """
    if not refresh and os.path.exists(_CACHE):
        with open(_CACHE, encoding="utf-8") as fh:
            return json.load(fh)
    with urllib.request.urlopen(TREE, timeout=120) as r:
        tree = json.load(r)
    names = [e["path"][len("Named_Boxarts/"):] for e in tree["tree"]
             if e["path"].startswith("Named_Boxarts/")
             and e["path"].endswith(".png")]
    with open(_CACHE, "w", encoding="utf-8") as fh:
        json.dump(names, fh)
    return names


def _norm(s):
    """Fold a title to something comparable across naming conventions."""
    s = os.path.splitext(s)[0].lower()
    s = re.sub(r"\([^)]*\)|\[[^\]]*\]", " ", s)      # (USA), [!] ...
    s = s.replace("&", " and ")
    s = re.sub(r"\bthe\b|\ba\b", " ", s)
    s = re.sub(r"[^a-z0-9]+", " ", s)
    return " ".join(s.split())


def _score(a, b):
    """Token overlap, 0..1.  Deliberately simple: ROM names vary wildly and a
    fuzzy edit distance matched 'Sonic Advance' to 'Sonic Advance 2' too
    readily.  Requiring shared tokens in BOTH directions is stricter."""
    ta, tb = set(a.split()), set(b.split())
    if not ta or not tb:
        return 0.0
    inter = len(ta & tb)
    return inter / max(len(ta), len(tb))


_PENALTY = ("virtual console", "demo", "kiosk", "beta", "proto",
            "sample", "promo", "gba video")
_REGION = ("(usa)", "(usa, europe)", "(world)", "(usa, australia)",
           "(europe)", "(usa, europe, japan)")


def _preference(name):
    """Tie-break between scans of the same game.  Region tags are stripped
    before scoring, so every localisation ties at 1.00 -- and picking the
    SHORTEST name then chose 'Golden Sun (Italy)' over '(USA, Europe)'.
    Rank by release first, then by region, then by brevity."""
    low = name.lower()
    for bad in _PENALTY:
        if bad in low:
            return (2, 99, len(name))          # a different box entirely
    for i, reg in enumerate(_REGION):
        if reg in low:
            return (0, i, len(name))
    return (1, 50, len(name))


def match_roms(rom_dir, names, threshold=0.72):
    """[(rom_filename, thumbnail_name or None, score)] for every .gba present."""
    idx = [(_norm(n), n) for n in names]
    out = []
    for fn in sorted(os.listdir(rom_dir)):
        if not fn.lower().endswith((".gba", ".zip")):
            continue
        key = _norm(fn)
        best, best_n = 0.0, None
        for nk, nn in idx:
            s = _score(key, nk)
            if s > best:
                best, best_n = s, nn
            elif s == best and best_n and _preference(nn) < _preference(best_n):
                best_n = nn
        out.append((fn, best_n if best >= threshold else None, round(best, 3)))
    return out


def fetch_cover(thumb_name, dest_path, timeout=60):
    url = RAW + urllib.parse.quote(thumb_name)
    with urllib.request.urlopen(url, timeout=timeout) as r:
        data = r.read()
    with open(dest_path, "wb") as fh:
        fh.write(data)
    return len(data)

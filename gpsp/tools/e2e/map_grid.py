#!/usr/bin/env python3
"""map_grid.py -- print a walkability grid for a pret map (route planning).

Reads a pret checkout (map.json + layouts.json + map.bin +
metatile_attributes.bin) and prints one ASCII cell per tile.  Handles both
pokeemerald and pokefirered/pokeleafgreen, which differ in two ways
(Phase-4 FR/LG work, docs/AUTOPILOT.md):

  * metatile attributes are u16 (behavior = bits 0-7) in Emerald and **u32**
    (behavior = bits 0-8, `sMetatileAttrMasks` in FR src/fieldmap.c) in FR/LG;
  * the primary/secondary tileset split is 512 metatiles in Emerald and
    **640** in FR/LG (`NUM_METATILES_IN_PRIMARY`, FR include/fieldmap.h).

Both are auto-detected from the checkout, so the same invocation works for
either game.  Output is one ASCII cell per tile:

    .  walkable (collision 0, plain behavior)
    #  blocked (collision != 0)
    g  walkable but wild-encounter grass (tall/long grass) -- scripts avoid
    ~  water / waterfall / dive behaviors
    D  door / warp-ish behavior (walkable)
    O  object event home position (NPCs -- static block or wander origin)
    W  warp event
    v ^ < >  one-way ledge (MB_JUMP_SOUTH/NORTH/WEST/EAST): blocked against
             the arrow, hoppable with it

Coordinates are map-local (== SaveBlock1.pos units, no +7 MAP_OFFSET).
Usage: map_grid.py PRET_DIR MAP_NAME (e.g. map_grid.py ~/gpsp-e2e/pokeemerald MauvilleCity)
"""
import re
import json
import struct
import sys
from pathlib import Path

# metatile behavior ids (include/constants/metatile_behaviors.h)
GRASS = {0x02, 0x03, 0x07, 0x24, 0x25}          # tall/long/ash/underwater grass-ish
WATER = set(range(0x10, 0x20)) | {0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F}
DOORS = {0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A,
         0x6B, 0x6C, 0x6D, 0x6E, 0x6F, 0x71}   # 0x71 = MB_UNION_ROOM_WARP (FR/LG)
# One-way ledges. They carry collision 1, so a plain grid marks them blocked —
# correct for the direction you cannot go, badly wrong for the direction you
# can.  Route 1 in FR/LG is ledged across rows 5/10/15/20/26/31/36 and a
# southbound plan that reads them as walls is simply wrong (Phase-4: a leg
# aimed at a "wall" ran straight through it).  MB_JUMP_{EAST,WEST,NORTH,
# SOUTH} = 0x38..0x3B in both games.
LEDGES = {0x38: ">", 0x39: "<", 0x3A: "^", 0x3B: "v"}


def tileset_attr_path(root: Path, label: str) -> Path:
    # gTileset_General -> */general/metatile_attributes.bin (searches both tiers).
    # FR/LG also has trailing-digit names (gTileset_GenericBuilding1 ->
    # generic_building_1), so try the digit-separated spelling as well.
    name = label.replace("gTileset_", "")
    snake = "".join(("_" + c.lower()) if c.isupper() else c for c in name).lstrip("_")
    cands = [snake, re.sub(r"([a-z])(\d)", r"\1_\2", snake)]
    for cand in dict.fromkeys(cands):
        for tier in ("primary", "secondary"):
            p = root / "data" / "tilesets" / tier / cand / "metatile_attributes.bin"
            if p.exists():
                return p
    raise SystemExit(f"no metatile_attributes.bin for tileset {label} ({snake})")


def attr_format(root: Path, prim: bytes) -> tuple:
    """(entry_size, behavior_mask, primary_metatile_count) for this checkout.

    NUM_METATILES_IN_PRIMARY is 512 in pokeemerald and 640 in pokefirered;
    the attribute record is u16 there and u32 here.  Read the constant out of
    include/fieldmap.h and let the primary attribute file size settle the
    record width, so neither number is hardcoded per game.
    """
    n_primary = 0x200
    try:
        src = (root / "include" / "fieldmap.h").read_text()
        m = re.search(r"#define\s+NUM_METATILES_IN_PRIMARY\s+(\d+)", src)
        if m:
            n_primary = int(m.group(1))
    except OSError:
        pass
    size = 4 if len(prim) >= n_primary * 4 else 2
    return size, (0x1FF if size == 4 else 0xFF), n_primary


def main():
    root = Path(sys.argv[1]).expanduser()
    map_name = sys.argv[2]
    mj = json.load(open(root / "data" / "maps" / map_name / "map.json"))
    layouts = json.load(open(root / "data" / "layouts" / "layouts.json"))
    lay = next(l for l in layouts["layouts"] if l and l.get("id") == mj["layout"])
    w, h = int(lay["width"]), int(lay["height"])
    blocks = (root / lay["blockdata_filepath"]).read_bytes()
    prim = tileset_attr_path(root, lay["primary_tileset"]).read_bytes()
    sec = tileset_attr_path(root, lay["secondary_tileset"]).read_bytes()
    asize, amask, n_primary = attr_format(root, prim)
    afmt = "<I" if asize == 4 else "<H"

    def behavior(metatile):
        if metatile < n_primary:
            buf, idx = prim, metatile
        else:
            buf, idx = sec, metatile - n_primary
        if (idx + 1) * asize > len(buf):
            return 0
        return struct.unpack_from(afmt, buf, idx * asize)[0] & amask

    grid = []
    for y in range(h):
        row = []
        for x in range(w):
            v = struct.unpack_from("<H", blocks, (y * w + x) * 2)[0]
            meta, coll = v & 0x3FF, (v >> 10) & 3
            b = behavior(meta)
            if coll and b in LEDGES:
                c = LEDGES[b]
            elif coll:
                c = "#"
            elif b in GRASS:
                c = "g"
            elif b in WATER:
                c = "~"
            elif b in DOORS:
                c = "D"
            else:
                c = "."
            row.append(c)
        grid.append(row)

    for o in mj.get("object_events", []):
        x, y = int(o["x"]), int(o["y"])
        if 0 <= x < w and 0 <= y < h:
            grid[y][x] = "O"
    for wv in mj.get("warp_events", []):
        x, y = int(wv["x"]), int(wv["y"])
        if 0 <= x < w and 0 <= y < h:
            grid[y][x] = "W"

    print(f"{map_name}  {w}x{h}  layout={mj['layout']}  "
          f"(attr u{asize * 8}, primary={n_primary})")
    print("    " + "".join(str(x % 10) for x in range(w)))
    for y, row in enumerate(grid):
        print(f"{y:3d} " + "".join(row))
    for c in mj.get("connections") or []:
        print(f"connection: {c['direction']:6s} {c['map']} offset={c.get('offset')}")


if __name__ == "__main__":
    main()

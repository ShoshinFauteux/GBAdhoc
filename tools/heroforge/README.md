# HeroForge

Companion tools for GBAdhoc's game browser. Optional — the emulator works
without any of this, and the Shelf shell looks fine with box art alone.

```
heroforge.py       point it at a ROM folder; fetches box art and builds
                   Marquee wallpapers for the games you own
raw565.py          bake a PNG into the PSP's native texture format
install_heroes.py  install a downloaded hero pack, matched to your games
preview.py         render art under the real Marquee UI, to judge it
deband.py          repair a hard dark band across the bottom of generated art
compose.py         derive a wallpaper from box art (the no-AI path)
styles.py          alternative compositions
bake_font.py       (../) Inter -> coverage atlas
bake_logo.py       (../) the GBAdhoc wordmark -> coverage texture
```

Needs Python 3.8+ and Pillow: `pip install pillow`

## Where the art comes from

**Box art** is fetched from the public
[libretro-thumbnails](https://github.com/libretro-thumbnails) archive — the
same source RetroArch uses. Only for games already in your ROM folder; the
tool does not find, link to, or download games.

**Hero wallpapers** are 480x272 backdrops for the Marquee shell. Two ways to
get them:

1. **A hero pack**, downloaded separately and installed with
   `install_heroes.py`. These are generated images — see the provenance note
   below.
2. **Derived from box art** by `compose.py`, with no AI involved. Lower
   ceiling, but it works for any game and needs nothing but the cover.

## Why hero art is distributed separately

It is **not** in the emulator's repository, and that is deliberate.

Hero art is derived from copyrighted box art — the characters, palettes and
styles belong to Nintendo, Konami, Capcom and others. Bundling it would put
that risk on the emulator itself. RetroArch keeps thumbnails in a separate
archive for the same reason, which is why `libretro-thumbnails` exists as its
own project.

So the emulator stays clean and the art lives somewhere expendable. If a pack
ever has to come down, the emulator is untouched.

## Provenance

Hero packs are **generated images**, not scans and not official artwork. The
method is documented in `PROMPT.md`, which ships with each pack so anyone can
produce art for games a pack does not cover. The composition rules in it are
not stylistic preferences — they are where the shell draws its text, and they
were derived by looking at mockups that failed.

## Naming — this is the one rule that matters

**A hero file's name must match its ROM's name exactly, apart from the
extension.**

```
roms/Pokemon - Emerald Version (USA, Europe).gba
hero/Pokemon - Emerald Version (USA, Europe).png     <- correct
hero/Pokemon Emerald.png                             <- will not be found
```

Art is matched to games by filename and nothing else — no database, no
fuzzy lookup at runtime. A mismatched name is not an error: the game simply
shows no hero and falls back to its box art, which looks like the pack is
broken when it is not.

The same rule applies to `boxart/`.

If a pack's names do not line up with your ROMs, `install_heroes.py` will
re-match and rename them for you, and list anything it could not place.

## Formats

Drop `.png` files into `PSP/GAME/GBAdhoc/hero/`. Nothing else is needed.

The emulator decodes a PNG the first time it shows that game, then writes a
`.565` beside it — the GE's own texture format, ready to use. Every later
visit is a single read with no decode at all. Measured on the same image:
**112 ms as PNG, 14 ms as .565**.

That conversion is lazy: one game at a time, only for games actually looked
at, and only while the browser is idle. A large library never stalls at boot,
and there is no batch step to wait through.

`raw565.py` can pre-bake the whole set if you would rather not pay the first
view — it also dithers the 8-bit to 5/6/5 step, which the console's
truncation does not, so gradients come out slightly cleaner than the on-device
conversion produces.

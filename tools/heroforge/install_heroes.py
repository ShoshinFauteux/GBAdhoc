"""Install a hero-art pack onto a memory stick, matching it to the games present.

    python install_heroes.py <pack_dir> <psp_gbadhoc_dir> [--roms <dir>]

WHY THIS EXISTS RATHER THAN "unzip it into hero/".

Art is matched to a game by filename, and the filenames in a downloaded pack
will not always be the filenames on someone's card.  A pack made from No-Intro
names has "Pokemon - Emerald Version (USA, Europe).png"; a real card might hold
"Pokemon Emerald.gba", "pokemon_emerald.gba", or the same name with a different
region tag.  Unzipping puts the file in the right folder with the wrong name,
the emulator finds nothing, and the art silently does not appear -- the worst
failure mode there is, because it looks like the pack is broken.

So: match every pack image against the ROMs actually present, using the same
normaliser HeroForge uses for box art, and write it out under the ROM's name.
Anything that cannot be matched confidently is reported, never guessed.
"""
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import library                                   # noqa: E402

EXT = ('.png', '.jpg', '.jpeg', '.webp', '.565', '.bmp')


def install(pack_dir, app_dir, rom_dir=None, threshold=0.72):
    rom_dir = rom_dir or os.path.join(app_dir, 'roms')
    hero_dir = os.path.join(app_dir, 'hero')
    if not os.path.isdir(rom_dir):
        print('no roms/ under %s -- pass --roms' % app_dir)
        return 1
    os.makedirs(hero_dir, exist_ok=True)

    roms = [f for f in sorted(os.listdir(rom_dir))
            if f.lower().endswith(('.gba', '.zip'))]
    if not roms:
        print('no .gba files in %s' % rom_dir)
        return 1
    keys = [(library._norm(r), r) for r in roms]

    art = [f for f in sorted(os.listdir(pack_dir)) if f.lower().endswith(EXT)]
    print('%d games on the card, %d images in the pack' % (len(roms), len(art)))
    print()

    used, missed = set(), []
    for fn in art:
        k = library._norm(fn)
        best, best_rom = 0.0, None
        for rk, rname in keys:
            sc = library._score(k, rk)
            if sc > best:
                best, best_rom = sc, rname
        if best < threshold or not best_rom:
            missed.append((fn, best))
            continue
        stem = os.path.splitext(best_rom)[0]
        dst = os.path.join(hero_dir, stem + os.path.splitext(fn)[1].lower())
        shutil.copyfile(os.path.join(pack_dir, fn), dst)
        used.add(best_rom)
        note = '' if os.path.splitext(fn)[0] == stem else '   (renamed)'
        print('  %.2f  %-42s -> %s%s' % (best, fn[:42], os.path.basename(dst), note))

    print()
    if missed:
        print('NOT INSTALLED -- no game on the card matches these:')
        for fn, sc in missed:
            print('  %.2f  %s' % (sc, fn))
        print()
    have = [r for r in roms if r in used]
    lack = [r for r in roms if r not in used]
    print('%d of %d games now have hero art.' % (len(have), len(roms)))
    if lack:
        print('Still without art (the shell falls back to the box cover):')
        for r in lack[:20]:
            print('  %s' % r)
    return 0


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    rom = (sys.argv[sys.argv.index('--roms') + 1]
           if '--roms' in sys.argv else None)
    return install(sys.argv[1], sys.argv[2], rom)


if __name__ == '__main__':
    sys.exit(main())

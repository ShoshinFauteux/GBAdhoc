# Feature requests and bug reports from the 3.0.0 Reddit thread (2026-10-01)

Source: r/PSP "GBAdhoc 3.0.0 released" (t3_1wrw037), about 70 of 108 comments
exported. Overall sentiment: overwhelmingly positive (100% upvote ratio, about
1,585 points, no negative comments).

## Bugs (fix first)

| Bug | Reporter | Ease / risk | Status |
|---|---|---|---|
| Audio drifts out of sync after minutes; opening the menu resets it (1000, PRO-C; Unbound, FE Sacred Stones, Sonic Adv. 2; also a FE hack) | Notnbutgravity, HairSalty5626 | medium / low | root-caused (docs/SA2-TITLE.md §2): the audio ring settles at 0.75 s after ~3.5 min; target cut to 125 ms on claude/sa2-title, hardware check pending |
| GB palettes: Red's Flamethrower wrong colour, Crystal too dark, left-edge blur in Blue/Yellow | Queasy_Reference254 | easy-medium / low | claude/gb-palette (docs/GB-PALETTE-FIXES.md): Crystal fixed (GBC LCD model); Flamethrower is the game's own Super Game Boy palette (no bug); left edge not reproduced, a right/bottom-edge filter fringe fixed; ask the reporter for a screenshot |
| Sonic Advance 2 title: white background instead of ocean; bilinear filter delays audio | Hecyo800 | medium / low | title: 3.0.0 ME contract, fixed by ME_AFFINE_LINES + ME_MIDFRAME_LOG (candidate); audio: the ring lag above, not the filter (docs/SA2-TITLE.md); hardware check pending |
| Link's Awakening slows down in the opening house (1000) | Notnbutgravity | medium / low | open |
| Some itch.io GB/GBC homebrew won't boot | Notnbutgravity | medium / low | open |
| Crash after loading gbadhoc_me.prx as a CFW plugin | Spyda_V | easy / none | README: "do not add it as a plugin" |
| DBZ Supersonic Warriors wireless: partner never appears | Spyda_V | hard / medium | per-game RFU compatibility |

## Feature requests, ranked easiest and lowest risk first

1. Control remapping, including unbinding Triangle: in progress (claude/control-remap).
2. Per-console display profiles (GB/GBC/GBA): in progress (claude/display-features).
3. Integer 2x scaling: in progress (claude/display-features).
4. "Back to game list": already exists (Start+Select → Quit to game list); fix the README.
5. Ambient mode, with blurred hero art in the side bars: in progress (claude/display-features + Fable mockups).
   Also the hero art behind the ROM loading screen (owner request).
6. Sharp-bilinear / sharpening filter: next; cost needs measuring on the 1000.
7. Cheats: medium risk (memory patches vs SMC); needs oracle tests.
8. Online via XLink Kai / Adhoc2USB (2 requests): test whether RFU survives 50-100 ms.
9. GB/GBC link cable: done (candidate).
10. Wireless for non-Pokemon games: per-game RFU; the GBA link mirror covers cable games.
11. Vita port / Adrenaline: not feasible (no Media Engine).
12. NDS; talking to a real GBA or Switch: out of scope.

# The in-process return on real hardware: the soak

Branch `claude/inproc-soak` (= `claude/inproc-return` e4be91c, which already
contains `claude/candidate-3.1-all` 77aaabb, so the merge was a no-op),
2026-10-03. This answers docs/INPROC-RETURN.md §7 items 1-5 on the PSP-1000
and the PSP-3000. Items 6 (fallback after a real wireless session) and 7
(sleep) were not exercised: no refusal ever happened.

**Verdict: safe to ship.** Both consoles ran 230 in-process returns each,
and the PSP-3000 ran 60 more with H&S held resident (§5): 520 in total, about
2 h of console time. There were no refusals and no census
mismatches. Every heap census after an unload was byte-identical to the
process's first. Every one of the 474 sessions was frame-identical to a fresh
boot of the same game after the state load. There were no badjumps, no
bjrec, no stall records and no lost events. The Media Engine came up and
rendered in every GBA session, and HOME loaded in every session.

## 1. The rig

One solo `hw_loop` per console (`tools/inproc/hw/run_soak.sh`), from the GB
link rig's app folder `PSP/GAME/GBADHOC-GBLINK`, which was backed up first
(freezerig `--scope PSP/GAME/GBADHOC-GBLINK`; the backups are kept off-repo).
The soak's files all live in `roms/soak/`
under `s_` names. The rig's own saves are restored from `golden/` after every
run, and the last cycle stages arm Z, which puts back the rig's own EBOOT, ME
prx, CONFIG.INI and harness ini, then sends STOP. Both consoles were left
parked.

| console | build | EBOOT md5 | ME prx |
|---|---|---|---|
| PSP-1000 (D:, join), 32 MiB layout | harness | `8a9420edaa9be06ee76b63bac7c9ffad` | `18158678` |
| PSP-3000 (F:, host) | harness64 | `601a2366cd6d0dbb8ca71123ffa8e8c8` | `19ae57b6` |

HOME prx `d3434f4f`. LAYOUT_PIN=1, JIT_CODE_DISCIPLINE=1 (the candidate's
profile). CONFIG.INI: `menu_button = home`, `me_mode = 1`.

Arms (`tools/inproc/hw/make_stages.py`), in this order on each console:

| arm | what |
|---|---|
| S | 100 returns, harness quit: aw2 → red.gb → emerald → crystal.gbc → hns, x20. Each game loads its slot-1 state at frame 30 and runs 300 frames. `shash = 1`, `vhash = 1`, `me_mode = 1`. Also installs the EBOOT, the prx files and the ROMs. |
| U | 30 returns through the real UI. The browser's state shelf picks each game (LEFT, X on slot 1), and the in-game menu's own Quit to game list ends it (`inproc_via_menu`). aw2/emerald/hns, and each return logs `ui_return fade_done ms`. |
| R E C H | fresh boots of red / emerald / crystal / hns: the identity references (aw2's is S's session 0) |
| T | as S, reverse order (hns → crystal → emerald → red → aw2) |
| Z | the rig's own files back, STOP |

Every arm ran in PPSSPP first, from the same stage directories
(`tools/inproc/hw/smoke.sh`, Xvfb :145/:146). The PPSSPP runs also serve as
the cross-reference in §3.

## 2. Results

`tools/inproc/hw/soak_analyse.py` (its full output per console is not kept
in the repo).

| | PSP-1000 | PSP-3000 |
|---|---|---|
| sessions / in-process returns | 237 / 230 | 237 / 230 |
| refused (fallback relaunch) | 0 | 0 |
| census `pre_select` / `post_select` / `post_unload` equal to the first | 237/237, 237/237, 230/230 | 237/237, 237/237, 230/230 |
| `post_load` heap per game | one value per game, every time | one value per game, every time |
| ME `me_init state=up` | 155/155 GBA sessions (GB/GBC run without it) | 155/155 |
| HOME `home_service state=up` | 237/237 | 237/237 |
| badjump / bjrec / stall.txt | 0 / 0 / 0 | 0 / 0 / 0 |
| `evt_drop` | 0 in every run | 0 in every run |
| vhash after the state load vs the fresh boot | 234/234 sessions identical | 234/234 |
| shash after the state load vs the fresh boot (GBA) | 152/152 identical | 152/152 |
| one soak run (101 sessions) | 839 s, i.e. ~8.3 s per game → browser → game cycle, 300 frames of play included | 855 s |

The census value repeats exactly on each model:
- PSP-1000: `free=15154632`-class (32 MiB layout).
- PSP-3000: `free=48709064`-class.
- The kernel partition (`kfree`/`kmax`) never moved.

So the 230th game on each console loaded into the same heap a fresh boot
gives it. Frames 1-30 of a GBA boot differ even between two fresh boots
(boot noise, docs/INPROC-RETURN.md §5): emerald shash f4-30, hns f11-30, and
vhash f1 on some sessions. After the state load, nothing differs.

### Return time (arm U, Quit → the browser accepting input, 16-frame crossfade included)

| | PSP-1000 | PSP-3000 |
|---|---|---|
| all (n=30) | min 925, median 1127, max 1218 ms | min 984, median 1151, max 1184 ms |
| after aw2 / emerald / hns | 1134 / 1134 / 967 ms (median) | 1168 / 1151 / 1017 ms |

For comparison, a fresh boot of the same EBOOT takes 2.97 s (1000) and 3.23 s
(3000) from EBOOT entry to the browser (`boot_timeline browser=`). The relaunch
this replaces costs that plus the firmware's LoadExec teardown and reload,
which no log can see and which shows a black screen.

## 3. Hardware vs PPSSPP

The same arms ran in PPSSPP with the same binaries:

- **shash** after the state load is identical to PPSSPP's fresh boot in every
  GBA session on both consoles (guest state is bit-exact on hardware).
- **vhash** is identical for GB/GBC.
- **GBA vhash** differs from PPSSPP on a fixed set of frames per game (aw2
  68, emerald 41, hns 16 of 271). The hardware's own fresh boots differ in
  exactly the same frames. That is the real Media Engine renderer against
  PPSSPP's CPU renderer (PPSSPP fails the ME handshake), not the return.

## 4. Findings

1. **H&S is not resident on a real PSP-3000 with the LARGE JIT tier: it
   misses by one block.** The fresh boot logs `rom_cache ... plan=heap_short
   want=32 got=31` and pages at 15 blocks. PPSSPP's 64 MiB heap is larger
   (ppsspp-heap-5mib-over-go), so the x20's "hns resident" never happens on
   this console. In this soak the 3000's resident-ROM path was exercised by
   Emerald: 16 blocks, the whole cart, in 40 + 15 sessions. A follow-up arm X
   (`jit_small = 1`, so H&S is resident, 60 returns, §5) covers the 32 MiB
   resident case.
2. The PC disk filled twice during the run (other agents' docker builds).
   hw_loop crashed mid-collect both times; `run_soak.sh <model> <order>
   <runs> <suffix>` resumes a chain. `run_disp.sh` no longer copies `roms/`
   back out of each PPSSPP sandbox: 60 MB per run made the smoke 1 GB.
3. The browser lists PPSSPP's file names upper-cased (`SOAK/S_AW2.GBA`), and
   the PSP-3000's too, so a `last_rom` written by hand may not match. The UI
   script therefore starts from row 0.

## 5. Arm X: H&S resident on the PSP-3000

`run_soak_x.sh` (arms X then Z with STOP), with the same EBOOT and
`jit_small = 1`. With the SMALL tier the 3000 holds H&S whole: `rom_cache
blocks=32 ... plan=resident want=32 got=32`, `post_load used=34692984` in
all 13 H&S sessions, 12 of them after an in-process return. The order was
hns → aw2 → red → emerald → crystal, 60 returns, states as in S.

| | PSP-3000, arm X |
|---|---|
| sessions / returns / refused | 61 / 60 / 0 |
| census | 61/61, 61/61, 60/60 equal to the first: exact |
| ME up / HOME up | 37/37 GBA / 61/61 |
| badjump / bjrec / stall / evt_drop | 0 / 0 / 0 / 0 |
| identity after the state load | every session identical to the console's fresh boot |

The fresh-boot references are the LARGE-tier, paged boots of arms S/E/H, so
this also shows that guest state and the presented frames do not depend on
the tier or on ROM residency. Covered here: a 32 MiB resident cart, the
whole D/I cache flush at unload, the JIT re-emitting into the same SMALL
cache 60 times, and the ME restarted per game. No derail.

PSP-3000 including X: 298 sessions, 290 returns, 0 refused, census exact in
all 8 runs, 0/295 vhash and 0/189 shash sessions different after the state
load.

# AUTOPILOT.md — input-script engine + verified RAM/ROM address tables

Two target families are documented here: **Emerald (BPEE rev 0)** — the Gate-3
table, verified end to end — and **FireRed/LeafGreen (BPRE/BPGE rev 1)** —
Phase-4 cross-edition work, verified as far as the harness can currently
reach. Jump to "FR/LG (BPRE/BPGE rev 1)" below for the second table, its
provenance, and the explicit list of what is still unverified and why.

**THE PSP CONTROL FILE IS `.gpsp-harness.ini`, NOT `autopilot.ini` (ADR-0036).** The engine
kept its name; the *file* was renamed because it could auto-host, auto-join and skip the ROM
browser, and a stale copy on a memory stick did exactly that three times — most recently
bringing a wireless session up during a solo benchmark. A leftover `autopilot.ini` is now
ignored and reported as `EVT legacy_autopilot_ignored`; it is never deleted. Anything below
that says "autopilot" means the *input-script engine* (`ap_loaded`/`ap_done`/`ap_fail`), which
is unchanged.

Phase-1 work product (plan §7.1 step 3). The engine lives in
`frontend-common/fe_autopilot.[ch]` and runs identically in both frontends
(SDL twin: `--script FILE [--ff]`; PSP: `.gpsp-harness.ini` keys
`script = <file>` / `ff = 1`). Script grammar is documented in
`fe_autopilot.h`; committed scripts live in `testdata/fixtures/*.inputs`
(the only testdata content allowed into git — DECISIONS.md user decision 1).

## How RAM predicates reach emulated memory

- **EWRAM (0x02000000, 256 KiB)** — `retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM)`
  returns the live `ewram` data half directly (FRONTEND-AUDIT §6).
- **IWRAM (0x03000000, 32 KiB)** — *not* exposed via `retro_get_memory_data`;
  read through the `RETRO_ENVIRONMENT_SET_MEMORY_MAPS` descriptors the core
  registers at `retro_load_game` (libretro/libretro.c `set_memory_descriptors`:
  IWRAM ptr=`iwram`, offset 0x8000, start 0x03000000, len 0x8000; EWRAM
  start 0x02000000, len 0x40000). `fe_host_mem_read()` resolves both, with a
  plain-EWRAM fallback if descriptors were never received.
- Reads happen between `retro_run` calls (engine steps run right before the
  next frame), so values are always frame-coherent.
- **Function pointers read from RAM carry the Thumb bit** — a predicate
  comparing `gMain.callback2` against a pret symbol must use
  `symbol_address | 1`. All values below are listed both ways.

## Target ROM identity

`testdata/Pokemon - Emerald Version (USA, Europe).gba`: header game code
`BPEE` (0xAC), **revision byte 0x00** (0xBC) — i.e. Emerald USA rev 0,
exactly the ROM pret/pokeemerald rebuilds. Symbol source:
`pret/pokeemerald` `symbols` branch, `pokeemerald.sym` (fetched 2026-07-31;
73,204 symbols; matches the rev-0 US ROM). Struct offsets cross-checked
against pret source (`include/main.h` struct Main, `include/global.h`
struct SaveBlock1/WarpData, `src/start_menu.c` save-dialog state machine).

## Verified address table (Emerald BPEE rev 0)

| Symbol (pret) | Address | Size | Use / predicate | Verified |
|---|---|---|---|---|
| `gMain.callback2` (`gMain`+4) | `0x030022C4` | u32 | current main callback — the game-state discriminator | D+P |
| title `MainCB2` (title_screen.c, local) | ROM `0x080AAB2C` → cb2 == `0x080AAB2D` | — | title screen running | D+P |
| `CB2_MainMenu` (local) | ROM `0x0802F6B0` → cb2 == `0x0802F6B1` | — | main menu (CONTINUE/NEW GAME) running | D+P |
| `CB2_Overworld` | ROM `0x08085E5C` → cb2 == `0x08085E5D` | — | in-field (start menu & save dialog run as tasks under it) | D+P |
| `gMenuCallback` | `0x03005DF4` | u32 | == `HandleStartMenuInput\|1` while the start menu accepts input | D+P |
| `HandleStartMenuInput` (local) | ROM `0x0809FAC4` → `0x0809FAC5` | — | see above | D+P |
| `sStartMenuCursorPos` (local) | `0x0203760E` | u8 | start-menu cursor index (logged; SAVE == 5 with the full 8-item menu) | D+P |
| `sSaveDialogCallback` (local) | `0x0203761C` | u32 | 0 until the save flow first runs; then the save-dialog state | D+P |
| `SaveConfirmSaveCallback` (local) | ROM `0x080A00A0` → `0x080A00A1` | — | save dialog just engaged (observed value after selecting SAVE) | D+P |
| `SaveReturnSuccessCallback` (local) | ROM `0x080A02D8` → `0x080A02D9` | — | terminal save-dialog state ("...saved the game." shown; auto-dismisses after a 60-frame timer — `SaveSuccesTimer`) | D+P |
| `gSaveBlock1Ptr` | `0x03005D8C` | u32 | → SaveBlock1 (Emerald relocates it; always deref) | D+P |
| SaveBlock1 `+0x00` `pos` | via ptr | 2×s16 | player x,y (compared save-run vs verify-run) | D+P |
| SaveBlock1 `+0x04` `location` | via ptr | WarpData | mapGroup/mapNum/warpId | D+P |

"Verified" column: **D** = desktop twin live run 2026-07-31
(`~/gpsp-e2e/desktop-save/run1.log`/`run2.log` in WSL: every `waitram`
predicate fired, and `logram`/`logptr` values matched — `cb2_title=0x080aab2d`,
`cb2_mainmenu=0x0802f6b1`, `cb2_overworld=0x08085e5d`, `menucb=0x0809fac5`,
`cursor=0x05`, `savecb=0x080a00a1`; restored overworld visually confirmed via
BMP dump). **P** = PSP EBOOT in PPSSPP via `tools/e2e/run_save_test.sh` /
`run_soak.sh` (same scripts, same predicates, artifacts in
`tools/e2e/artifacts/`).

## Save-flow notes (from pret `src/start_menu.c`, relied on by the scripts)

- Opening the start menu sets `gMenuCallback = HandleStartMenuInput`; the
  cursor wraps, so **UP×3 from any fresh boot (cursor 0) lands on SAVE**
  for both the 6-item and 8-item menu layouts (empirically: 8 items,
  cursor 5).
- With an existing save the flow is: confirm Yes/No (default Yes) →
  "already saved, overwrite?" Yes/No (default Yes) → "SAVING…" →
  `TrySavingData(SAVE_NORMAL)` (this is when SRAM CRC changes) →
  "…saved the game." → `SaveReturnSuccessCallback` auto-completes after 60
  frames (A not required — scripts avoid pressing A here so a stray press
  can't talk to an adjacent NPC/sign after the menu closes).
- `sSaveDialogCallback` and `gMenuCallback` are **stale after their flows
  end** (never reset to 0) — predicates must compare against specific
  values, not "became nonzero again"; the scripts only use the ==0→!=0
  transition for the very first save-dialog engagement of a boot.
- SRAM-write detection is game-agnostic: `waitsram`/`mashsram` poll the
  frontend's CRC32 of the 128 KiB SRAM buffer (`fe_host_sram_crc_now`)
  instead of a game symbol.

## Gate-3 grammar additions (Phase 3)

The trade e2e needed four new step families (grammar in `fe_autopilot.h`):

- `waitptr`/`waitptrne`/`mashptr` — deref predicates: read a u32 GBA pointer,
  evaluate `(mem[*ptr+off]&mask)==val`. Needed for Emerald's relocating
  `gSaveBlock1Ptr` and for heap/pointer-reached state blocks
  (`sWirelessLinkMain.uRoom->state`). An invalid/NULL pointer just evaluates
  false — safe across map loads and frees.
- `holdram`/`holdptr` — hold buttons continuously until a predicate holds
  (gen-3 movement: tap = turn, hold = walk), used for closed-loop
  navigation on live coordinates.
- `holdmash HBTNS MBTNS …` / `holdmashptr` — hold + pulsed second button
  set. The movement workhorse is `holdmash <DIR> B …`: keeps walking while
  the B pulses dismiss Emerald's step-triggered Pokénav match calls
  (risk-register row 11 — a call otherwise freezes the walk and times the
  predicate out; B is inert in the field so it can never interact).

## Gate-3 address-table additions (Emerald BPEE rev 0)

All verified live in the desktop-twin trade runs 2026-07-31/08-01
(`tools/e2e/artifacts/trade-*`), same pret provenance as above.

| Symbol (pret) | Address | Size | Use / predicate | Verified |
|---|---|---|---|---|
| `gObjectEvents[0].currentCoords.x/.y` | `0x02037360` / `0x02037362` | s16 | live player tile (+7 MAP_OFFSET vs map coords) — `SaveBlock1.pos` only updates on warp/save, so all walking predicates use these | D |
| `gObjectEvents[0].mapNum/mapGroup` | `0x02037359` | u16 | current map as `mapNum \| mapGroup<<8` (object view) | D |
| `gPlayerAvatar.objectEventId` | `0x02037595` | u8 | == 0 in every observed state (player owns object slot 0) | D |
| SaveBlock1 `+0x04` loc u16 | via `0x03005D8C` | u16 | map as `mapGroup \| mapNum<<8`; updates on warps AND connection crossings (`ApplyCurrentWarp`). Values used: Route 117 `0x2000`, Mauville `0x0200`, PC1F `0x050A`, PC2F `0x060A`, Union Room `0x3C19` | D |
| `gPlayerPartyCount` | `0x020244E9` | u8 | party size | D |
| `gPlayerParty` | `0x020244EC` | 6×100B | slot N personality u32 at `+N*0x64` — the trade oracle | D |
| `gEnemyParty` | `0x02024744` | — | received-mon staging during link trade (logged) | D |
| `gPlayerCurrActivity` | `0x02022C2C` | u8 | `0x44` = ACTIVITY_TRADE\|IN_UNION_ROOM | D |
| `sWirelessLinkMain` | `0x02022C30` | u32 | union of heap pointers; `*ptr+0x14` = `WirelessLink_URoom.state` (UR_STATE_*: 4 MAIN, 52 REGISTER_REQUEST_TYPE — full enum in pret `src/union_room.c`) | D |
| `sUnionRoomTrade` | `0x02022C40` | 0x18 | board registration state (not predicated on; documented for completeness) | D |
| `gReceivedRemoteLinkPlayers` | `0x03003124` | u8 | 1 while a game-level link is established | D |
| `CB2_UpdatePartyMenu` (local) | ROM `0x081B01B0` → `0x081B01B1` | — | party menu active (mon selection steps) | D |
| `CB2_LinkTrade` | ROM `0x0807AE50` → `0x0807AE51` | — | the trade sequence began — both scripts' rendezvous predicate | D |
| `CB2_PrintErrorMessage` (local, link.c) | ROM `0x0800B1A0` → `0x0800B1A1` | — | the game's own "Communication error…" dialog (terminal screen `CB2_LinkError` transitions into) — the `--disconnect` survival predicate | D |

Union-Room geometry used by the scripts (pret map JSONs via
`tools/e2e/map_grid.py`): entrance (7,11); registration nurse (3,2) talked
to from (3,3); trading board tile (2,1) faced from (2,2); the 8 avatar
spots are `sUnionRoomPlayerCoords` ± `sUnionRoomGroupOffsets` (plus-shapes
around (4,6),(13,8),(10,6),(1,8),(13,4),(7,4),(1,4),(7,8)) — idle mashing
must never face one of those tiles or the attendant (the host idles at
(3,4) facing (3,5), and waits **hands-off** on `waitptrne state!=MAIN`
until contacted; a stray A while an avatar spawns can start an unwanted
interaction).

## Address maintenance

These addresses are constants of the BPEE rev-0 ROM image — they cannot
drift under emulator/frontend changes, only under a *different ROM*
(other revision or region). `run_boot_test.sh` logs the ROM code and the
scripts' first predicate (title) fails fast on a mismatched image;
`run_trade_test_psp.sh` now also checks the header game code (0xAC) and the
**revision byte (0xBC)** of every ROM it is about to run, because the tables
are per-revision constants and a swapped image otherwise surfaces as a
mystifying predicate timeout hundreds of frames in.

---

# FR/LG (BPRE/BPGE rev 1)

## Target ROM identity — verified

| file | title (0xA0) | code (0xAC) | rev (0xBC) | md5 |
|---|---|---|---|---|
| `testdata/Pokemon - FireRed Version (USA, Europe) (Rev 1).gba` | `POKEMON FIRE` | **BPRE** | **0x01** | `51901a6e40661b3914aa333c802e24e8` |
| `testdata/Pokemon - LeafGreen Version (USA, Europe) (Rev 1).gba` | `POKEMON LEAF` | **BPGE** | **0x01** | `9d33a02159e018d09073e700e1fd10fd` |

Both are **revision 1**, which is what DECISIONS.md ADR-0009 pins — so the
symbol source must be the rev-1 build, not the rev-0 one. Getting that wrong
is the trap the ADR warns about and it is not a subtle failure: FR rev0's
`CB2_InitCopyrightScreenAfterBootup` is at a different address from rev1's,
so the very first predicate would time out.

## Symbol provenance

`pret/pokefirered` (one repo builds all four images), commit `df4449a`,
**`symbols` branch**, files `pokefirered_rev1.sym` (50,807 symbols) and
`pokeleafgreen_rev1.sym` (50,809) — fetched 2026-08-01, same mechanism as the
Emerald table's `pokeemerald.sym`. Struct offsets cross-checked against pret
source (`include/main.h` struct Main, `include/global.h` SaveBlock1/WarpData,
`include/global.fieldmap.h` ObjectEvent/PlayerAvatar, `include/union_room.h`
WirelessLink_URoom, `src/start_menu.c`, `src/union_room.c`). Map ids are
computed from `data/maps/map_groups.json` (group id = index in
`group_order`, map num = index within the group, both in file order).

**That the .sym matches the ROM is itself an empirical result, not an
assumption.** The first callback the FR ROM ever installs was read live as
`0x080EC835`, which is exactly `CB2_InitCopyrightScreenAfterBootup | 1` from
`pokefirered_rev1.sym` — and *not* LeafGreen's (`0x080EC80C`), so the same
read also proves the file is matched to the right one of the two games.

## Verified address table (FR/LG BPRE/BPGE rev 1)

FR and LG are the same build with a small text/data delta, so **almost every
address is identical between them**; the handful that are not are called out
explicitly. Function-pointer compares carry the Thumb bit, as in Emerald.

| Symbol (pret) | FireRed | LeafGreen | Size | Use / predicate | Verified |
|---|---|---|---|---|---|
| `gMain.callback2` (`gMain`+4) | `0x030030F4` | same | u32 | the game-state discriminator | D |
| `CB2_InitCopyrightScreenAfterBootup` | ROM `0x080EC834` → `…835` | **`0x080EC80C`** → `…80D` | — | first callback after boot; the .sym-matches-ROM proof | D |
| `CB2_SetUpIntro` / `CB2_Intro` (local) | `0x080EC884` / `0x080EC9E8` | −0x28 | — | intro movie (observed, not predicated on) | D |
| `CB2_InitTitleScreen` | ROM `0x08078928` → `…929` | same | — | title screen entering | D |
| `CB2_TitleScreenRun` (local) | ROM `0x08078BB0` → `…BB1` | same | — | **title screen running** — every script's first predicate | D |
| `CB2_MainMenu` (local, main_menu.c) | ROM `0x0800C2E8` → `…2E9` | same | — | main menu (CONTINUE/NEW GAME) running | D |
| `CB2_Overworld` (local) | ROM `0x080565C8` → `…5C9` | same | — | in-field | D |
| `CB2_NewGameScene` (local) | `0x0812EB88` | **`0x0812EB60`** | — | Oak's speech / new-game intro | D |
| `CB2_LoadNamingScreen` (local) | `0x0809D9F4` | **`0x0809D9C8`** | — | naming screen loading | D |
| `CB2_NamingScreen` (local) | ROM `0x0809FB84` → `…B85` | **`0x0809FB58`** → `…B59` | — | naming keyboard running | D |
| `BattleMainCB2` | ROM `0x08011114` → `…115` | same | — | a battle is running | D |
| `gSaveBlock1Ptr` | `0x03005008` | same | u32 | → SaveBlock1 (FR/LG relocates it too; always deref) | D |
| SaveBlock1 `+0x00` `pos` | via ptr | via ptr | 2×s16 | player x,y | D |
| SaveBlock1 `+0x04` `location` | via ptr | via ptr | WarpData | map as `mapGroup \| mapNum<<8` | D |
| SaveBlock1 `+0x0430` `bagPocket_PokeBalls[0].itemId` | via ptr | via ptr | u16 | `4` = ITEM_POKE_BALL (a "got the balls" oracle) | D |
| `gSaveBlock2Ptr` | `0x0300500C` | same | u32 | → SaveBlock2 (player name/ID; not predicated on) | D |
| `gObjectEvents[0].currentCoords.x/.y` | `0x02036E48` / `0x02036E4A` | same | s16 | live player tile (**+7 MAP_OFFSET** vs `SaveBlock1.pos`, same as Emerald) | D |
| `gObjectEvents[0].facingDirection` | `0x02036E50` | same | u8 | low nibble: 1=S 2=N 3=W 4=E | D |
| `gObjectEvents[0].mapNum/mapGroup` | `0x02036E41` / `0x02036E42` | same | u8 | current map, object view | D |
| `gPlayerAvatar.objectEventId` | `0x0203707D` | same | u8 | == 0 in every observed state | D |
| `gPlayerPartyCount` | `0x02024029` | same | u8 | party size | D |
| `gPlayerParty` | `0x02024284` | same | 6×100B | slot N personality u32 at `+N*0x64` — the trade oracle | D |
| `gEnemyParty` | `0x0202402C` | same | — | received-mon staging during a link trade | S |
| `sStartMenuCallback` (local) | `0x020370F0` | same | u32 | == `StartCB_HandleInput\|1` = `0x0806F295` while the start menu accepts input; becomes the *chosen action* callback on A (observed `StartCB_Save2` `0x0806F5DD`) | D |
| `sStartMenuCursorPos` (local) | `0x020370F4` | same | u8 | start-menu cursor index | D |
| `sNumStartMenuItems` (local) | `0x020370F5` | same | u8 | item count — **5** with neither Pokédex nor Pokémon flag, **6** with `FLAG_SYS_POKEMON_GET`, **7** with both. Doubles as a cheap `FLAG_SYS_POKEDEX_GET` assertion | D |
| `sSaveDialogCB` (local) | `0x03000FA4` | same | u32 | **IWRAM** (Emerald's equivalent is EWRAM). 0 until the save flow first runs | D |
| `SaveDialogCB_ReturnSuccess` (local) | ROM `0x0806F9F4` → `…9F5` | same | — | terminal save-dialog state | D |
| `SaveDialogCB_AskSavePrintYesNoMenu` (local) | `0x0806F7F0` | same | — | early save-dialog state (observed) | D |
| `gWirelessCommType` | `0x03003F3C` | same | u8 | 0 offline (only the offline value observed) | D |
| `sPlayerCurrActivity` (local) | `0x0203B058` | same | u8 | `ACTIVITY_TRADE\|IN_UNION_ROOM` = `0x44` | S |
| `sWirelessLinkMain` (local) | `0x0203B05C` | same | u32 | union of heap pointers; `*ptr+0x14` = `WirelessLink_URoom.state`, **same offset as Emerald** (`include/union_room.h`) | S |
| `sUnionRoomTrade` (local) | `0x0203B06C` | same | 0x18 | board registration state | S |
| `gReceivedRemoteLinkPlayers` | `0x03003F64` | same | u8 | 1 while a game-level link is established | S |
| `gLinkPlayers` / `gLocalLinkPlayer` | `0x0202273C` / `0x02022720` | same | — | link roster (not predicated on) | S |
| `CB2_UpdatePartyMenu` (local) | `0x0811EC18` | **`0x0811EBF0`** | — | party menu active. **FR≠LG** — the FR/LG trade scripts deliberately avoid it and predicate on the UR state instead | S |
| `CB2_LinkTrade` | ROM `0x0805014C` → `…14D` | same | — | the trade sequence began — both scripts' rendezvous predicate | S |
| `CB2_PrintErrorMessage` (local, link.c) | ROM `0x0800AF40` → `…F41` | same | — | the game's own "Communication error…" dialog — the fatal-error oracle | S |
| `CB2_LinkError` | `0x0800ACE8` | same | — | the screen `CB2_PrintErrorMessage` runs under | S |
| `Task_RunUnionRoom` (local) | `0x08118758` | **`0x08118730`** | — | the union-room task (not predicated on; listed because it is FR≠LG) | S |

**Verified column.** **D** = desktop twin live run 2026-08-01 on the real
ROMs — the value was read out of emulated RAM and matched, or a `waitram`/
`waitptr` predicate on it fired. Evidence: `tools/e2e/artifacts/` plus the
`testdata/fixtures/*_stage*.inputs` runs described below; the FR run reached
a two-mon party and the LG run reached the CONTINUE path, and every EVT
`ap_val` in those logs resolves to a symbol in the rev-1 `.sym`.
**S** = *symbol-derived only, NOT yet observed at runtime.* Every S row is a
union-room / link / trade address, and they are all in the same bucket for
one reason: reaching them requires a save already parked at a wireless
counter with a Pokédex and two party mons, which this repo does not have for
FR/LG yet. **What promotes them:** one `run_trade_test_psp.sh
--roms=firered,leafgreen` against user-supplied parked saves. A wrong S value
fails loudly as a `waitptr`/`mash` timeout at a named `evt` mark, never as
silent wrong behaviour — the FR/LG trade scripts are written so each S
address sits behind its own labelled predicate.

## FR/LG map ids (SaveBlock1.location as `mapGroup | mapNum<<8`)

| map | id | verified |
|---|---|---|
| `MAP_UNION_ROOM` (group 0 `gMapGroup_Link`, num 4) | `0x0400` | S |
| `MAP_TRADE_CENTER` (group 0, num 1) | `0x0100` | S |
| `…_POKEMON_CENTER_2F` (Viridian: group 5, num 5) | `0x0505` | S |
| `…_POKEMON_CENTER_1F` (Viridian: group 5, num 4) | `0x0405` | D |
| `MAP_VIRIDIAN_CITY` (group 3, num 1) | `0x0103` | D |
| `MAP_VIRIDIAN_CITY_MART` (group 5, num 3) | `0x0305` | D |
| `MAP_ROUTE1` (group 3, num 19) | `0x1303` | D |
| `MAP_PALLET_TOWN` (group 3, num 0) | `0x0003` | D |
| `…_PLAYERS_HOUSE_1F` / `_2F` (group 4, num 0 / 1) | `0x0004` / `0x0104` | D |
| `…_PROFESSOR_OAKS_LAB` (group 4, num 3) | `0x0304` | D |

Note that Pokémon Center 2F is per-city in FR/LG (every city's 2F is its own
map sharing `LAYOUT_POKEMON_CENTER_2F`), so the 2F id depends on which
Center the fixture is parked in — unlike Emerald, where the harness could
hard-code Mauville's `0x060A`. The FR/LG trade scripts therefore never
predicate on the 2F id; they only assert the Union Room id at the end.

## Where FR/LG differ structurally from Emerald

Not assumed-equal — each of these was checked against pret source, and the
first four bit during Phase-4 bring-up.

1. **Start-menu state.** Emerald exposes `gMenuCallback` (IWRAM `0x03005DF4`)
   which equals `HandleStartMenuInput` while the menu takes input. FR/LG has
   no such symbol: the equivalent is `sStartMenuCallback` in **EWRAM**
   (`0x020370F0`) holding `StartCB_HandleInput`, and it becomes the *selected
   action* on A. FR/LG also exposes `sNumStartMenuItems`, which Emerald does
   not, and which is the cleanest "is the menu up, and how many rows" probe.
2. **Save-dialog state lives in IWRAM.** `sSaveDialogCB` is `0x03000FA4`
   (IWRAM); Emerald's `sSaveDialogCallback` is EWRAM `0x0203761C`. A script
   ported by search-and-replace on the *value* would still read EWRAM and
   silently never fire.
3. **Start-menu composition.** Emerald's 8-item field menu puts SAVE at
   index 5 and the cursor wraps, so `UP×3` works. FR/LG builds
   `[POKéDEX] [POKéMON] BAG PLAYER SAVE OPTION EXIT` — SAVE is at index
   **3** before the Pokédex and **4** after it. The stage scripts press DOWN
   the right number of times and then assert `sStartMenuCursorPos`.
4. **`gPlayerCurrActivity` is global in Emerald, static in FR/LG**
   (`sPlayerCurrActivity` `0x0203B058`).
5. **Union Room map id**: Emerald `0x3C19`, FR/LG `0x0400`.
6. **Union Room and Pokémon Center 2F geometry are identical** — this one is
   a happy non-difference and worth recording as checked rather than
   assumed. The registration nurse is at (3,2) (talk from (3,3)); the
   trading board is at (2,1) faced from (2,2) — hard-coded in
   `union_room.c IsPlayerFacingTradingBoard` as `(2 + MAP_OFFSET,
   1 + MAP_OFFSET)` in both games; the entry warp is (7,11); the 2F Union
   Room attendant is at (6,2) with an `MB_COUNTER` at (6,3), so the player
   stands at (6,4) facing north. The `UR_STATE_*` enum is also identical
   (MAIN = 4, REGISTER_REQUEST_TYPE = 52, REGISTER_SELECT_MON = 54), as is
   `WirelessLink_URoom.state` at `+0x14`.
7. **Entering the Union Room is gated in FR/LG.**
   `CableClub_EventScript_UnionRoomAttendant` (pret
   `data/scripts/cable_club.inc:738`) requires `FLAG_SYS_POKEDEX_GET`, no Bad
   Egg, `IsWirelessAdapterConnected`, then
   `CountPartyNonEggMons >= 2` and no Enigma Berry, and only then runs
   `EventScript_AskSaveGame`. Its prompt is `MULTICHOICE_YES_NO_INFO` —
   **three** rows (YES/NO/INFO), not Emerald's two. A parked FR/LG save that
   fails any of these bounces at the counter.
8. **Directional stair warps.** FR/LG house stairs are
   `MB_{UP,DOWN}_{LEFT,RIGHT}_STAIR_WARP` and
   `IsDirectionalStairWarpMetatileBehavior` only fires for `DIR_WEST`/
   `DIR_EAST` — walking *onto* the tile from below does nothing. Confirmed
   live: standing on PlayersHouse_2F (10,2) never warped; arriving moving
   west did.
9. **Ledges.** Route 1 is crossed by `MB_JUMP_SOUTH` rows; they carry
   collision 1 (so a naive grid prints them as walls) but are hoppable
   southbound. `tools/e2e/map_grid.py` now renders them as `v`/`^`/`<`/`>`.
10. **FR ≠ LG address splits.** Only six of the symbols the harness cares
    about differ: `CB2_InitCopyrightScreenAfterBootup`, `CB2_SetUpIntro`,
    `CB2_Intro`, `CB2_NewGameScene`, `CB2_LoadNamingScreen`,
    `CB2_NamingScreen` (all −0x28 in LG), plus `CB2_UpdatePartyMenu` and
    `Task_RunUnionRoom`. Everything the trade scripts predicate on is
    identical, which is why `frlg_trade_host.inputs` /
    `frlg_trade_join.inputs` are single files shared by both games — the one
    place that would have needed a split (`CB2_UpdatePartyMenu`) is
    sidestepped by predicating on the UR state instead.

## Trade legality in FR/LG (the pre-National-Dex rule)

`src/trade.c CanTradeSelectedMon` and
`src/union_room.c IsRequestedTradeInPlayerParty` between them impose three
rules that a fixture has to respect:

- **Never your last mon.** `numMonsLeft` sums every party slot except the
  one offered; 0 → `CANT_TRADE_LAST_MON`. Eggs are zeroed out first, so
  "one mon + one egg" also fails. This is the same requirement the Union
  Room attendant enforces at the door (`CountPartyNonEggMons >= 2`).
- **Kanto dex only, without the National Dex.**
  `species2[monIdx] > KANTO_SPECIES_END` → `CANT_TRADE_NATIONAL`, and
  symmetrically if the *partner* lacks the National Dex. Eggs are rejected
  either way (in the vanilla build via a known pret-documented off-by-one
  that routes them into the same branch).
- **The trading board's "requested type" is screened against the whole
  party**, using base species types from `gSpeciesInfo`, not against the mon
  you eventually pick — it only gates *entering* the trade flow.

**What the FR/LG scripts therefore ask for, and why:** the host registers
requesting **TYPE_NORMAL** (row 0 of the request-type list, so no cursor
movement). Any plausible early FR/LG party contains something Normal-typed —
a Route 1 Pidgey is Normal/Flying and a Rattata is Normal/Normal — so NORMAL
is the most permissive request and the one least likely to bounce with
"doesn't have the type". A Kanto starter plus an early-route catch clears all
three rules with no National Dex, which is why the fixture contract only asks
for two non-egg Kanto-dex mons rather than anything more specific.

## Empirical-verification harness (`*_stage*.inputs`) — retained, not a fixture path

`testdata/fixtures/firered_stage1.inputs`, `leafgreen_stage1.inputs`,
`frlg_stage2.inputs`, `frlg_stage3.inputs`, `frlg_stage4.inputs` play a
**brand new game** from the title screen to a two-mon party on Route 1,
chained through the `.sav` between stages (the engine caps a script at 256
steps — `frontend-common/fe_autopilot.c AP_MAX_STEPS`, not ours to change —
and the route is several times that).

They are **not** the fixture path and are not wired into any test script:
parked FR/LG saves are supplied by hand instead (that decision is ADR-0022).
They are kept because they are the evidence behind the **D** rows above —
running them is how each of those addresses was observed — and because the
chain stops one leg short of a wireless counter, which is exactly the leg
that was dropped. Reproduce with `sdl/gpsp_sdl --rom <fr|lg>.gba --save
S.sav --script <stage>.inputs --ff`, stages in order, same `.sav` throughout.
Observed on FireRed: stage 1 → starter Squirtle + rival battle + save
(17,612 frames); stage 2 → Route 1 north, Oak's Parcel, save (25,144); stage
3 → Pokémon Center heal, Route 1 south, Pokédex + 5 Poké Balls, save
(21,561); stage 4 → Mom heal, Route 1 grass, **Rattata caught**, party
count 2, save (11,500). At uncapped FF the desktop twin runs ~5,500 fps, so
the whole chain is a few seconds of wall clock.

Two things learned there are worth keeping even though the path is dropped:
a wild battle can start on any grass step, so a leg that holds a direction
until a coordinate predicate is unrecoverable if a battle interrupts it (a
held direction only moves the battle action cursor and the `holdmash` then
just times out); and the party must be healed before each Route 1 crossing,
because a lone starter whites out after ~6 wild battles and the whiteout
silently teleports the run to the respawn point.

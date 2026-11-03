# Share game data

Save files and save states from one PSP to another over ad-hoc wireless.
Browser only: neither PSP has a game loaded.

| | |
|---|---|
| Protocol, matching, file safety | `frontend-common/fe_share.c` / `.h` |
| Host test | `tools/tests/test_fe_share.c` (in the `fe_ini_bounds` suite) |
| Radio, stick, build identity | `psp/share_psp.c` / `.h` |
| Screens | `psp/ui_share.inc` (included by `psp/ui_psp.c`) |
| Room kind | `psp/room_label.h` (`ROOM_SHARE`, `GSnnDATA`) |
| Two-PPSSPP rig | `tools/share/run_share_ppsspp.sh`, `mkscript.py` |

## What the player does

**Sharing PSP.** ROM browser > START > WIRELESS > **Share game data**. The
PSP reads its `roms/` folder, opens a data-share room on the Settings room
number and shows a waiting screen (what it offers, whether a PSP has joined,
what is being sent). O stops. Nothing else is asked of it.

**Other PSP.** ROM browser > START > WIRELESS > **Join: scan for rooms** --
the same room list the in-game Wireless menu has, which the browser could
not reach before. The share room is listed as `Room 7 / Game data / saves
and states`; X joins it as any room is joined, and because the room is a
data share its menu opens by itself. There is no other entry on this side.

1. **Game data** (per console): the sharer's games that this PSP also has,
   in the browser's order, each row saying what is offered (`SAVE - 3
   states`). TRIANGLE switches console with the browser's own flare and
   rescan. Games this PSP does not have are not listed.
2. X opens **Their states** for that game over its hero art: a Save file row
   above Slots 1-5, the highlighted slot's picture 1:1 on the right. The
   save row shows the hero art in the frame with both dates on a runner.
3. X on a slot opens **Your slots** (this PSP's first five slots with their
   thumbnails, the incoming picture on the right); X there fetches the state
   into the chosen slot. X on the Save file row fetches the save, after one
   question if this PSP already has a save for the game ("Your current save
   is kept as a backup").
4. After a fetch the page is Their states again, with a line saying what
   landed and a mark on the row (`got it` / `replaced yours`). O returns to
   the game list, which shows what was taken. The marks live in the session
   and are gone when the room is left (O on the list).

With a game open, the scan list shows a share room dimmed ("Room 7 is
sharing game data. Join it from the game list."); from the browser, link
rooms are dimmed the other way round.

## Room kinds

A scan returns only 8-byte group names, so the name says what kind of host
it is (`psp/room_label.h`):

| Name | Kind |
|---|---|
| `GPSPnn` | Wireless Adapter room |
| `GCnnXXXX` / `GBnnXXXX` | Link Cable room, GBA / GB |
| `GSnnDATA` | **Share game data** room |

One discovery path (the existing scan and list) serves all of them.

**Builds that do not know `GSnnDATA`.** They list the name verbatim and let
the player join it as a link room. Nothing in a share room speaks the link
protocol: the sharer never starts netdrv, takes only datagrams that begin
with `GSHR`, and treats everything else as noise. So the old build finds
nobody to link with -- an adapter session with no partner, or a cable
handshake that times out -- and the sharer goes on sharing. Tested with the
3.5.x beta harness build as the joiner (`run_share_ppsspp.sh OUT old`).

## Protocol

One datagram per message on the transport's bulk lane (magic `GSHR`), little
endian, at most 1052 bytes:

    "GSHR" | proto u8 | type u8 | session u16 | body | crc32

| Type | Direction | Body |
|---|---|---|
| HELLO | guest, broadcast | MAC, nonce, build string, state format x3 |
| WELCOME | sharer | the same of its own + entry / save / state counts, manifest size and CRC |
| REFUSE | sharer | reason (protocol generation, busy), its protocol number, its build |
| STAT / INFO | guest / sharer | object -> status, size, CRC32 of the whole file |
| GET | guest | object, first chunk, 32-bit map of the chunks wanted |
| DATA | sharer | object, chunk number, up to 1024 bytes |
| PING / PONG, BYE | | keep-alive; leaving |

* **Version.** `proto` is the protocol generation. A HELLO of another
  generation is answered with REFUSE, whose layout is frozen, so the guest
  can say "update both" and name the sharer's build instead of timing out.
  The build string and the per-console state format ids travel in
  HELLO/WELCOME; they decide whether states are offered (below).
* **Objects are indices, never names.** `entry u16 | kind u8 | slot u8`:
  an index into the sharer's own manifest, a kind (manifest, save, state,
  thumbnail, full preview) and a slot. The sharer maps that to a path
  through its own table. There is no kind for a ROM.
* **Receiver-driven.** The guest asks for a window of up to 32 chunks and
  re-asks for whatever is missing after 250 ms without data. The sharer
  holds one open file and one pending window; no datagram loss can corrupt
  its state. It sends `share_burst` (4) DATA datagrams per frame.
* **Cheap listing.** WELCOME carries the counts; the manifest (120 bytes a
  game: identity, name, the save's size and date, each slot's size, date and
  which previews exist) is the first fetch. Thumbnails (5 KiB) are fetched
  when a game's page is open, the cursor's first; the full preview (82 KiB)
  after the cursor has rested on a slot; a state or save only when chosen.
* **Timeouts.** HELLO is repeated every 250 ms for 12 s. A linked guest that
  hears nothing for 6 s reports the sharer lost; the sharer forgets a silent
  guest after 8 s. One guest at a time: a second is REFUSEd as busy.

### What a hostile guest can do

Nothing to the sharer's files. Its I/O interface has `open` (read-only),
`read` and `close` and no write. Every request is checked against the
manifest the sharer built itself (`fe_share_obj_valid`): an entry past the
end, a slot outside 1-5, a preview the slot does not have, a chunk past the
end of the file, a wrong length, a wrong session, a bad CRC are dropped or
answered with an error. The host test throws 300,000 forged and random
datagrams at the server and asserts that it never asks the platform to open
anything the manifest does not offer.

## Which game is which

A manifest entry carries a 16-byte identity, not a file name
(`fe_share_game_id`): the ROM's size, a CRC32 of its header block (GBA
0xA0-0xBF: title, game code, maker, version, complement; GB/GBC
0x134-0x14F: title, CGB flag, cartridge type, sizes, version, both
checksums), a CRC32 of four 256-byte samples spread through the image, and
the code characters for the log. Two copies of a game match whatever they
are called; a hack that keeps its base game's header (most Pokemon hacks
keep `BPRE`/`BPEE`) differs in size or in the samples and does not match.

The guest only opens ROMs whose size equals a shared game's (the directory
scan already has every size), so matching costs a handful of short reads,
not one per ROM. The sharer finds its data files by walking `roms/` once per
console (sizes and dates come with the directory entries; no per-file stat).

## File safety on the guest

1. Every fetch goes to `<final name>.gsp`, never to the final name.
2. When the last chunk is in, the guest reads the temp file back and
   compares the CRC32 of the whole payload with the sharer's (computed over
   the sharer's file before the first chunk left). A mismatch -- including
   the sharer's file changing mid-transfer -- fails the fetch.
3. Only then `fe_share_commit`:
   * **save**: the existing save is renamed to `<save>.bak` (the previous
     `.bak` is removed first: exactly one per game, the same name the
     pre-link backup uses), the temp is renamed in; if that fails the old
     save is renamed back.
   * **state**: the same with a `.old` that is removed afterwards (states
     are not backed up). Its thumbnail and full preview arrive before the
     commit and follow it; a state that has none removes the slot's old
     pictures.
4. A cancelled, failed or interrupted fetch removes its temp files. The
   guest's files are untouched until step 3.

A full Memory Stick fails at step 1/2 (`FE_SHARE_E_WRITE`).

## State compatibility

States are offered only when `fe_share_states_ok` says so: the same state
format id for that console (`GBA_STATE_VERSION`; a constant for the GB
core) **and** -- `FE_SHARE_STATES_NEED_SAME_BUILD`, default 1 -- the same
build string. Otherwise the five slots are greyed with the reason and the
Save file row works as usual. Share offers and fills slots 1-5 only: a
PSP has ten since UI-OVERLAY.md section 15.1, but the manifest's five per
game is the wire format, and `.st5`-`.st9` are not shared. Saves are plain cartridge data and always
cross. A GB/GBC save is copied byte for byte, RTC trailer included, exactly
as moving the file by USB would.

## Memory

Nothing is reserved at boot and nothing runs unless one of the two rows is
chosen. While sharing: the entry table, name pool and manifest (about 250
bytes a game with data, at most 1024 games) plus the transport's 46 KiB
bulk ring. While joined: the entry table, two sets of five 64x64 thumbnails
(80 KiB), one 82 KiB preview buffer, the bulk ring, and the browser's own
512 KiB hero texture. All of it is freed on leaving.

What it costs when it is NOT used is its code: the release image grows by
40,960 bytes (13,285,744 -> 13,326,704 at a0658e4, `GBA_LINK=1 tools/build.sh
release`), which is 40 KiB less heap at boot on a PSP-1000 (10,810,808 ->
10,769,848). The feature's objects and `ui_share.inc` are built `-Os`; at
`-O2` it was 48 KiB. About 26 KiB of it is the screens.

## Switches

| Where | What |
|---|---|
| `FE_SHARE_STATES_NEED_SAME_BUILD` (`fe_share.h`) | 1: states need the same build. 0: the same state format is enough. |
| `SHARE_LIST_FOLLOWS_SHELL` (`ui_share.inc`) | 1: the game list has the hero backdrop in Marquee and a flat page in Shelf. 0: always the hero. |
| `FE_SHARE_ID_SAMPLES` (`fe_share.h`) | content samples in the identity (4). |
| harness `share_burst` | DATA datagrams per frame (4). |
| harness `share_build` | pretend to be another build. |
| harness `share_fail_write_after` | temp files stop growing after N bytes. |

## Testing

`python tools/run_host_tests.py --only fe_ini_bounds` runs the protocol
test. `tools/share/run_share_ppsspp.sh OUT full|drop|mismatch|full_stick|liar|old`
runs two PPSSPP consoles, drives both with `ui_browser_script`, dumps every
screen and compares the sticks afterwards (`OUT/verdict.txt`).

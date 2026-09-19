# Mystery Gift delivery repair

The phone now uploads a Wonder Card **and its redemption scripts**. The local
distribution cart completes the games' RFU handshake and MysteryGiftLink
exchange. FireRed, LeafGreen and Emerald receive and save the gift themselves;
the delivery person runs the saved field script to grant the item and flags.
Production code never patches a ROM or writes gift data directly into a save.

## Defects repaired

- Defer CONNECT_ACK until the next frontend frame. The core enters CONNECTING
  after its send callback returns, so a synchronous response was ignored.
- Complete librfu NI name/join traffic before UNI commands. Child subframes
  have two-byte headers; parent headers have three. Parent UNI carries five
  14-byte player slots, including the child's echoed command without its
  rotating counter bits. A command array alone is not an RFU subframe.
- Exchange player IDs and LinkPlayerBlock, and honor standby/close barriers.
  Emit one parent carrier per frame; do not consume a fragment when a barrier
  occupies that frame's command slot.
- Reassemble bounded link blocks and MGL messages, validating identity, size
  and CRC. The games' CRC starts at `0x1121`, uses reflected polynomial `0x8408`,
  and **inverts the final result**. The old C and Kotlin helpers missed that
  inversion. The empty-input checksum is `0xEEDE`.
- Read the 100-byte GAME_DATA structure correctly: game code is at offset 92.
  Match the compatibility fields and English ROM code before choosing a script.
- Send an actual field script. Card bytes alone do not award a ticket. Send a
  deterministic 995-byte padded script buffer because the client copies its
  fixed buffer size when installing the saved script.
- Preserve the game's duplicate-card and discard-card confirmation flows.
  A malformed transfer disconnects; missing traffic/progress is bounded. Human
  discard prompts remain allowed to wait while the game polls.
- Report phone upload acceptance separately from game reception and saving.
  Identical parcel retries do not reset the cart; another parcel receives busy
  during a live transfer. An empty station does not advertise a gift.

## Interfaces and wire format

`psp/mgift_payload.{c,h}` validates the UDP parcel. `mgift_net.{c,h}` owns AP
association and its nonblocking socket. `mgift_cart.{c,h}` owns the frame-driven
distribution protocol. `main_psp.c:mgift_frame` connects these components.
The RFU core and ad-hoc trading transport are unchanged by this repair.

MGC2, all integers little-endian:

| Offset | Bytes | Meaning |
|---|---:|---|
| 0 | 4 | `MGC2` |
| 4 | 4 | Parcel ID: CRC32 of bytes 8 through end |
| 8 | 2 | Card length, 332 |
| 10 | 2 | FRLG script length, 0–512 |
| 12 | 2 | Emerald script length, 0–512 |
| 14 | 2 | Reserved, zero |
| 16 | 332 | WonderCard |
| 348 | variable | FRLG script, then Emerald script |

Maximum datagram size: 1372 bytes. A zero script length means unsupported.
Reply: `MGA2`, parcel ID u32, result u32 (0 accepted, 1 busy, 2 invalid).
The phone checks the ID and PSP source port before accepting a reply.
Ports remain phone 5394 / PSP 21064. MGC1/MGOK is superseded; update both apps.

Android owns the card and script catalogue in `GiftCards.kt` / `GiftBundle.kt`.
Scripts use the games' relocatable `setvaddress`/`v*` commands, not absolute ROM
addresses. They grant the item only once, set its receipt and travel flags only
after successful bag insertion, and permit retry when the bag is full.

| Gift | FireRed / LeafGreen | Emerald |
|---|---|---|
| Aurora Ticket | Supported | Supported |
| Mystic Ticket | Supported | Supported |
| Old Sea Map | Rejected in game | Supported |

These are station-authored English event scripts, not archived Nintendo event
payloads. Other languages, ROM hacks and other gift types are not validated.
The Aurora Card icon uses internal Deoxys species 411, not National Dex 386.

## Evidence and reproducibility

References: [pret FireRed/LeafGreen](https://github.com/pret/pokefirered/tree/c75f352304d529f6ba92d4f74b9cf8b5c3810788)
and [pret Emerald](https://github.com/pret/pokeemerald/tree/5eff78649e7170a877b961ef0b3da13b81a16038).
Relevant files are `src/librfu_rfu.c`, `src/link_rfu_2.c`,
`src/mystery_gift_{client,link,scripts}.c`, `src/script.c`, `src/util.c`,
`asm/macros/event.inc`, `include/global.h`, and `include/constants/flags.h`.
Both FR and LG are built from pret/pokefirered.

- Android: 52 unit tests pass, including UDP handover and a field-script
  interpreter checking all supported gift/game combinations, one-time receipt
  and full-bag retry.
- `python3 tools/run_mgift_tests.py`: production parser/cart under ASan+UBSan,
  using all three Android-emitted parcels. Covers corruption/truncation,
  deferred ACK, NI/UNI framing, CRC rejection, compatibility, duplicate cards,
  discard/cancel, timeouts and reconnects.
- Desktop interpreter core with the user's copied ROMs and disposable saves:
  FireRed/Aurora, LeafGreen/Mystic and Emerald/Old Sea Map reach **Save completed**.
  Post-transfer save inspection confirms exact card bytes, the correct padded
  script, and valid card/script checksums. The historical GBA ABI rounds
  RamScriptData to 1000 bytes for its checksum, including final padding.
- FireRed's actual delivery-person event grants item 371 once, sets flags
  `0x2A7`, `0x84B`, `0x3D8`, and reports already received on a second interaction.
  Desktop tests do not establish physical PSP Wi-Fi or Android hotspot behavior.
- FF configuration/scheduling regression passes. Existing FF artifact/profile
  changes remain included in the build previously validated by the user.

Local evidence is under `builds/mgift-diagnosis/` and `builds/mgift-fix1/` in
the containing workspace. ROMs, BIOS, save files and screenshots from those
tests are not added to source control. Only the disposable test fixtures had
the Mystery Gift menu unlock flag enabled; user device saves were not altered.

PSP recipe/toolchain is the pinned build in `FF-ARTIFACT-FIX.md`, with
`GIT_VERSION=ecdd8bc-mgift1` and title **GBAdhoc Gift Fix 1**. This identifier
means checkpoint plus the recorded working-tree patch, not a new commit.
Use the freshly built `psp/me/gbadhoc_me.prx`, never a stale top-level copy.

## Hardware acceptance

1. Use the matching release APK and separately labeled **GBAdhoc Gift Fix 1**.
2. Enable an open 2.4 GHz Mobile Hotspot named **Mystery Gift** and the PSP WLAN
   switch. Start the emulator's Wireless → Mystery Gift listener.
3. Select the gift in the phone app and distribute. **Ready in emulator** means
   uploaded, not saved in the game.
4. In the game's own MYSTERY GIFT menu, receive a Wonder Card through WIRELESS
   COMMUNICATION. Wait for **Save completed** before returning to the game.
5. Talk to the delivery person upstairs in a Pokémon Center, then save normally.
   Confirm the item and destination availability when normal story prerequisites
   are satisfied. Repeated interaction should not grant a duplicate item.
6. Repeat on each PSP and each supported title; exercise discard/cancel and a
   second transfer. Physical-device acceptance remains pending user testing.

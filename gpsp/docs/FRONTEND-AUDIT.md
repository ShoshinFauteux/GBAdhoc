# FRONTEND-AUDIT — what a native frontend must provide to host the gpSP libretro core

Phase-0 work product (plan §3.3.3). Audited against the pinned checkout
(libretro/gpsp master @ 5b6e751, branch `phase0-recon`). All citations are
`path:line` relative to the repo root
(`c:\Users\DrSto\OneDrive\Desktop\GBA PSP Wirless Trading (Claude)\gpsp-adhoc`).
PSP build = `make platform=psp1` → `gpsp_libretro_psp1.a`, CFLAGS include
`-DPSP -DSMALL_TRANSLATION_CACHE` and `HAVE_DYNAREC=1`, `CPU_ARCH=mips`
(`Makefile:237-249`), plus the global defaults `FRONTEND_SUPPORTS_RGB565=1`
(`Makefile:3`, applied at `Makefile:593-595`) and `-D__LIBRETRO__`
(`Makefile:566`). Env-command numeric IDs cited from the vendored
`libretro/libretro-common/include/libretro.h` (the source of truth we link
against).

---

## 1. Every `retro_environment` call the core makes

The core stores the env callback at `libretro/libretro.c:790`. Complete
enumeration of call sites, grouped by when they fire. "Safe false?" = can a
frontend return `false` (or not implement it) without breaking the core.

### 1.1 During `retro_set_environment` (called before `retro_init`)

| # | Command (id) | Call site | What the core does with the answer | Safe false? |
|---|---|---|---|---|
| 1 | `RETRO_ENVIRONMENT_GET_LOG_INTERFACE` (27, libretro.h:772) | libretro.c:792-795 | Stores `log_cb`; on false, `log_cb = NULL` and all logging is skipped (libretro.c:140-148) | Yes |
| 2 | `RETRO_ENVIRONMENT_GET_PERF_INTERFACE` (28, libretro.h:782) | libretro.c:797-806 | Pre-fills `perf_cb` with dummies, lets the frontend overwrite. Only used by `PERF_TEST` builds (libretro.c:557-575) and `perf_cb.perf_log()` at `retro_deinit` (libretro.c:721) — dummies make that a no-op | Yes |
| 3 | `RETRO_ENVIRONMENT_GET_VFS_INTERFACE` (45, libretro.h:1038) | libretro.c:808-811 | If true, routes all `filestream_*` I/O (ROM open/paging, BIOS load) through the frontend VFS via `filestream_vfs_init`. If false, libretro-common falls back to its built-in implementation, which on PSP compiles against `<pspiofilemgr.h>`/`<pspkernel.h>` (libretro-common/vfs/vfs_implementation.c:51-53, 73-75, 118-120) | Yes — recommended for our frontend (let the built-in PSP path do the I/O) |
| 4 | `RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION` (52, libretro.h:1124) | libretro/libretro_core_options.h:429 | version >= 1 → v1 options API; else legacy `SET_VARIABLES` | Yes (falls back to legacy) |
| 5 | `RETRO_ENVIRONMENT_GET_LANGUAGE` (39, libretro.h:961) | libretro_core_options.h:438 | Picks a translated option table (all non-English tables are NULL anyway, libretro_core_options.h:369-393) | Yes |
| 6 | `RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL` (54, libretro.h:1212) — or `SET_CORE_OPTIONS` (53, libretro.h:1150) if built with `HAVE_NO_LANGEXTRA`, or `SET_VARIABLES` (16, libretro.h:619) in the v0 fallback | libretro_core_options.h:442,444,535 | Registers the option definitions (§9). Core ignores the return value | Yes — options then just always read as unset and defaults apply (§1.3) |

### 1.2 During `retro_init`

| # | Command (id) | Call site | What the core does with the answer | Safe false? |
|---|---|---|---|---|
| 7 | `RETRO_ENVIRONMENT_GET_INPUT_BITMASKS` (51 \| EXPERIMENTAL, libretro.h:1112) | libretro.c:696-697 | Sets `libretro_supports_bitmasks`; selects bitmask vs per-button polling in `update_input` (input.c:78-101) | Yes — per-button path used |
| 8 | `RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE` (64, libretro.h:1396) probe with `NULL` | libretro.c:699-701 | Sets `libretro_supports_ff_override`; gates the R2 fast-forward hotkey (input.c:85-86,96-97) and the FF input descriptor (libretro.c:1155-1158) | Yes — FF hotkey simply disabled (our frontend owns FF anyway, plan §4.4) |
| 9 | `RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE` (78, libretro.h:1832) | libretro.c:703-704 | Hands the frontend `netpacket_iface` (libretro.c:546-554). Comment in core: "This interface is actually optional" | Yes for solo play; **mandatory for this project** — it is the entire multiplayer seam. Frontend must copy the struct and drive `start/receive/connected/disconnected/stop` |

### 1.3 During `retro_load_game` (and `check_variables`)

| # | Command (id) | Call site | What the core does with the answer | Safe false? |
|---|---|---|---|---|
| 10 | `RETRO_ENVIRONMENT_GET_VARIABLE` (15, libretro.h:611) ×14 keys | libretro.c:918-1118 (`check_variables`) | Reads each `gpsp_*` option (§9). On false/NULL the hardcoded default applies per-key (e.g. dynarec defaults ON, libretro.c:941-942; frameskip → `no_frameskip`, libretro.c:1037) | Yes — defaults are sane, but a frontend that wants official BIOS / serial control MUST answer `gpsp_bios` / `gpsp_serial` |
| 11 | `RETRO_ENVIRONMENT_SET_PIXEL_FORMAT` (10, libretro.h:568) with `RETRO_PIXEL_FORMAT_RGB565` | libretro.c:1193-1195 | On false only logs "RGB565 is not supported." and **continues anyway** — the psp1 build renders RGB565 unconditionally (§2) | Formally yes, practically no: frontend must accept RGB565 or colors are garbage |
| 12 | `RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY` (9, libretro.h:554) | libretro.c:1210-1211 | Directory searched for `gba_bios.bin` (libretro.c:1212-1214). On false, falls back to the ROM's own directory (`main_path`) | Yes (BIOS then expected next to the ROM) |
| 13 | `RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS` (11, libretro.h:579) | libretro.c:1155-1158 | Cosmetic labels only | Yes |
| 14 | `RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE` (23, libretro.h:719) | libretro.c:1268-1272 | Stores `rumble_cb`; if false, `rumble_cb=NULL` and the per-frame rumble block is skipped (libretro.c:1419-1424) | Yes |
| 15 | `RETRO_ENVIRONMENT_SET_MEMORY_MAPS` (36 \| EXPERIMENTAL, libretro.h:922) | libretro.c:1161-1173,1276 | Publishes IWRAM/EWRAM descriptors (§6). Return ignored | Yes |
| 16 | `RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK` (62, libretro.h:1357) | libretro.c:183,191,197 (`init_frameskip`, called from check_variables at libretro.c:1064-1067) | Registers occupancy callback for `auto`/`auto_threshold` frameskip. On false the core logs "Frameskip disabled…" and runs without auto frameskip (libretro.c:197-206) | Yes — but then only `fixed_interval` frameskip works |

### 1.4 During `retro_run` / runtime

| # | Command (id) | Call site | What the core does with the answer | Safe false? |
|---|---|---|---|---|
| 17 | `RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY` (63, libretro.h:1366) | libretro.c:1400-1405 | Asks frontend for ≥6 frames of audio latency whenever frameskip config changes (computed at libretro.c:209-221) | Yes |
| 18 | `RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE` (17, libretro.h:657) | libretro.c:1438-1439 | If true+updated → re-runs `check_variables(false)` for runtime-changeable options | Yes — options then only ever read at load |
| 19 | `RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE` (64) with real payload | libretro.c:388-410 (`set_fastforward_override`, driven from input.c:166-170) | Toggles frontend fast-forward when R2 held. Only fires if probe #8 succeeded | Yes (never fires if #8 returned false) |
| 20 | `RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO` (32, libretro.h:821) | libretro.c:78-86 (`gpsp_apply_sound_rate`) | Only on a *mid-session* `gpsp_sound_rate` change; renegotiates sample rate | Yes — avoid by fixing the sound rate before load |
| 21 | `RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION` (59, libretro.h:1313), `SET_MESSAGE_EXT` (60, libretro.h:1332), `SET_MESSAGE` (6, libretro.h:525) | libretro.c:150-167 (`show_warning_message`) | Warning toasts (bad BIOS, cheat errors, ROM-hack detection: libretro.c:1221,1228,1247,1255,1264,888-897). Return ignored | Yes — messages silently lost; our frontend should implement one of these to surface BIOS warnings |

**Mandatory minimum for the core to function at all:** none of the env calls
hard-fail the core — every call site tolerates `false`. The *practical*
mandatory set for this project: accept RGB565 (#11), answer `GET_VARIABLE`
(#10) for at least `gpsp_bios`/`gpsp_serial`/`gpsp_frameskip`, provide
`GET_SYSTEM_DIRECTORY` (#12) or co-locate the BIOS, and implement the
netpacket interface (#9).

Also non-env but required plumbing: the five `retro_set_*` callback setters
(libretro.c:816-831); `retro_set_audio_sample` may be a no-op (libretro.c:821
is empty — batch only).

---

## 2. Video

- **Pixel format: RGB565** on PSP. `retro_load_game` sets
  `RETRO_PIXEL_FORMAT_RGB565` (libretro.c:1193-1195). The renderer's palette
  conversion produces RGB565 unless `USE_XBGR1555_FORMAT` is defined
  (common.h:101-107), which only the PS2 target defines (`Makefile:451`).
  There is no XRGB8888 path anywhere. The frame buffer is 16-bit
  (`common.h:113-116`).
- **Dimensions/pitch:** 240×160, pitch = `GBA_SCREEN_PITCH * 2` = **480
  bytes** (common.h:109-111; passed at libretro.c:462-463 and 473-474).
  Geometry/timing: base=max=240×160, aspect 3:2, fps =
  `GBC_BASE_RATE/(308*228*4)` = 16777216/280896 = **59.7275 Hz**
  (libretro.c:55-56, 591-600; sound.h:75).
- **Frame dupe: YES — `retro_video_refresh` is called with `data=NULL`** when
  the core skips a frame (`skip_next_frame`), with width/height/pitch still
  valid (libretro.c:456-465). The PSP frontend must treat NULL as "re-present
  the previous frame".
- Normal frames pass `gba_screen_pixels` (or `gba_processed_pixels` when
  color-correction/frame-mixing post-processing is enabled, libretro.c:467-474).
  `gba_screen_pixels` is a plain `malloc` of `GBA_SCREEN_BUFFER_SIZE` =
  240×161×2 = 77,280 bytes (libretro.c:678-683; common.h:113-116 — one extra
  row reserved for winobj effects). **Note:** with frame-mixing enabled the
  core *swaps* `gba_screen_pixels` with a history buffer every frame
  (libretro.c:292-297, 321-325) — never cache the pointer across frames.
- **No PSP-special framebuffer path exists in the core.** There is no
  `#ifdef PSP` in libretro.c or video.cc; video.h exposes only
  `update_scanline`/`gba_screen_pixels` (video.h:23-29). The buffer is
  ordinary cached malloc memory — the frontend owns the GU upload (and must
  handle dcache writeback/coherency itself before GE reads it, since the core
  writes it with the CPU).
- `retro_load_game` refuses to load if the screen buffer allocation failed
  (libretro.c:1180-1185).

## 3. Audio

- **Format:** signed 16-bit interleaved stereo via
  `retro_audio_sample_batch_t` **only**; the per-sample callback setter is an
  empty stub (libretro.c:821-826).
- **Sample rate:** default **65536 Hz** (`sound_frequency =
  GBA_SOUND_FREQUENCY` = 64*1024; sound.c:26, sound.h:66), reported in
  `retro_get_system_av_info` (libretro.c:599). Option `gpsp_sound_rate` can
  select 32768 Hz (libretro.c:66-76; libretro_core_options.h:143-152) —
  attractive on PSP (halves mixing work; PSP hardware output is 44.1/48k so
  we resample either way).
- **Per `retro_run`:** `sound_frequency / 59.7275` ≈ **1097.25 stereo frames**
  (fractional accumulator at libretro.c:424-435), delivered in chunks of at
  most `AUDIO_BATCH_FRAMES_MAX` = 1024 frames per `audio_batch_cb` call
  (libretro.c:92, 443-453) — i.e. typically **two** batch calls per frame at
  65536 Hz (1024 + ~73), one at 32768 Hz (~549). The frontend batch callback
  must tolerate consecutive calls within one `retro_run`.
- Actual frames delivered can be less than nominal: `sound_read_samples`
  holds back the last 512 ring samples (sound.c:832-845). Frontend pacing
  should be tolerant (ring-buffer the output; plan §4.4 audio-master clock).

## 4. Input

- Device: `RETRO_DEVICE_JOYPAD`, port 0 only. Polled every `retro_run` via
  `input_poll_cb()` then `update_input()` (libretro.c:1332-1333).
- Buttons polled (btn_map, input.h:46-57): L, R, D-pad (up/down/left/right),
  Start, Select, B, A → the 10 GBA keys.
- **Extra/special buttons** (input.c:80-100):
  - `RETRO_DEVICE_ID_JOYPAD_X` = **Turbo A**, `JOYPAD_Y` = **Turbo B** —
    pulse train controlled by `gpsp_turbo_period` (input.c:103-126;
    input.h:64-69: default period 4, pulse width 2).
  - `RETRO_DEVICE_ID_JOYPAD_R2` = **Fast Forward hotkey**, only active when
    the frontend supports `SET_FASTFORWARDING_OVERRIDE` (input.c:85-86,96-97).
- Polling mode: single bitmask read via `RETRO_DEVICE_ID_JOYPAD_MASK` if
  `GET_INPUT_BITMASKS` succeeded, else 10+3 individual
  `input_state_cb` calls (input.c:78-101). Implement bitmasks on PSP — it is
  one `sceCtrlPeekBufferPositive` translation anyway.
- Keypad IRQ is derived inside the core (input.c:41-66); frontend supplies
  raw state only. `retro_set_controller_port_device` is a no-op
  (libretro.c:833).

## 5. Save RAM (`RETRO_MEMORY_SAVE_RAM`)

- **Backing buffer:** the static array `u8 gamepak_backup[1024*128]`
  (gba_memory.c:359-360; extern at gba_memory.h:312), returned directly by
  `retro_get_memory_data(RETRO_MEMORY_SAVE_RAM)` (libretro.c:1308-1311).
- **Size: always reported as 0x20000 (128 KiB)** regardless of actual backup
  hardware — "Assume 128KiB, biggest possible save" (libretro.c:1318-1321).
  Frontend persists the full 128 KiB; that matches RetroArch `.srm` behavior.
  (For `.sav` interop with other emulators, the *logical* size is
  64 KiB flash / 128 KiB flash / 32 KiB SRAM / 8 KiB or 512 B EEPROM per
  `backup_type`/`flash_bank_cnt` — gba_memory.c:2800-2812 shows the mapping.)
- The core pre-fills the buffer with 0xFF before the frontend's SRAM restore
  (libretro.c:1238), and normalizes all-zero saves to 0xFF at load
  (gba_memory.c:2800-2829). Frontend contract (standard libretro): load the
  saved file into the pointer *after* `retro_load_game` returns, and write it
  back on exit/periodically.
- **64K vs 128K flash detection for Pokémon:** two layers —
  1. **Game DB (`gba_over.h`)**, matched by 4-char gamepak code at
     `load_game_config_over` (gba_memory.c:1643-1697). Entries:
     - `"BPEE"` Pokémon Emerald — `FLAGS_FLASH_128KB | FLAGS_RTC |
       FLAGS_SERIAL | FLAGS_RFU` (gba_over.h:996-1004)
     - `"BPRE"` Pokémon Fire Red — same flags (gba_over.h:1169-1177)
     - `"BPGE"` Pokémon Leaf Green — same flags (gba_over.h:1214-1223)
     `FLAGS_FLASH_128KB` (defined gba_memory.c:1627) sets
     `flash_device_id = FLASH_DEVICE_SANYO_128KB` and
     `flash_bank_cnt = FLASH_SIZE_128KB` (gba_memory.c:1659-1662);
     `FLAGS_RFU` auto-selects `SERIAL_MODE_RFU` when `gpsp_serial=auto`
     (gba_memory.c:1673-1677 with FLAGS at gba_memory.c:1632-1636).
  2. **Fallback heuristics** when the code isn't in the DB: ROM string scan
     for `FLASH1M_V`/`FLASH512_V`/`SRAM_V`/`EEPROM_V` signatures plus a
     Pokémon-family header check that forces 128K Sanyo flash
     (gba_memory.c:2737-2883, `rom_is_pokemon_family` at 2785-2798). ROM
     hacks of the Pokémon engine also get forced 128K flash + RTC
     (gba_memory.c:2959-2971).
  No `game_config.txt` files are needed — that legacy mechanism does not
  exist in this codebase.
- **SRAM dirtiness: there is NO dirty flag or notification.** Writes land
  directly in `gamepak_backup` from the flash/SRAM/EEPROM write handlers
  (gba_memory.c:1082-1196, 523-590); grep for dirty/modified turns up nothing
  SRAM-related. **The frontend must hash or memcmp the 128 KiB buffer** on its
  flush timer (plan §4.5 anticipated this). A CRC32 of 128 KiB every ~5 s is
  negligible on PSP.

## 6. System RAM (`RETRO_MEMORY_SYSTEM_RAM`) — autopilot hook

- `retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM)` returns **`ewram` only**,
  size **0x40000 (256 KiB)** (libretro.c:1312-1313, 1322-1323). This is GBA
  EWRAM 0x02000000–0x0203FFFF, data-first: `ewram` is declared
  `u8 ewram[1024*256*2]` — the *first* 256 KiB is the live data, the second
  256 KiB is the SMC-detection shadow (gba_memory.h:270-271; the read map
  maps `ewram` base at gba_memory.c:2345). Offset within the block = GBA
  address − 0x02000000. Reads are safe any time between `retro_run` calls.
- **IWRAM is NOT exposed via `retro_get_memory_data`** — only through the
  `SET_MEMORY_MAPS` descriptors (libretro.c:1161-1173): descriptor 0 is IWRAM
  at GBA 0x03000000, len 0x8000, `ptr=iwram` with `offset=0x8000` (the data
  half of `u8 iwram[1024*32*2]`, whose *lower* 32 KiB is the SMC shadow —
  gba_memory.h:272, mapping `&iwram[0x8000]` at gba_memory.c:2346);
  descriptor 1 is EWRAM at 0x02000000, len 0x40000.
- **For our RAM-predicate autopilot:** since we host the core in-process we
  can consume the `retro_memory_map` we receive (implement env #15 by storing
  the descriptors), giving us both EWRAM and IWRAM predicates. pret symbols
  in EWRAM (most `gSaveBlock`/link state) also work through plain
  `retro_get_memory_data(SYSTEM_RAM)`.

## 7. Savestates

- `retro_serialize_size` returns the **constant** `GBA_STATE_MEM_SIZE` =
  416*1024 = **425,984 bytes** (libretro.c:840-843; savestate.h:98-99 — "this
  is an upper limit"). It never changes at runtime.
- `retro_serialize`/`retro_unserialize` **require `size ==
  GBA_STATE_MEM_SIZE` exactly** (libretro.c:845-862) — pass exactly this
  size. The buffer is zeroed then BSON-filled (libretro.c:850).
- **Serial/RFU coverage: essentially NOT serialized.** `gba_save_state`
  writes cpu + input + main + memory + sound sections only
  (savestate.c:178-182; checks at 129-140). The only serial-adjacent fields
  are in the `emu` document: `serial-irq-cycles`, `rand-state`, `gbp-state`
  (main.c:432-434). `rfu.c` and `serial_proto.c` contain zero savestate code
  (grep: no bson/savestate references in rfu.c). **Consequence:** loading a
  state while an RFU session is live desyncs the adapter model — the plan's
  §4.5 rule (block save AND load during sessions) is confirmed necessary from
  the code, not just prudence.

## 8. ROM loading & memory; PSP-1000 32 MB fit

### Loading path

- `retro_get_system_info` sets `need_fullpath = true` (libretro.c:585) — the
  frontend passes a path; the core opens the file itself
  (`load_gamepak_raw` → `filestream_open`, gba_memory.c:2604-2610) and keeps
  the handle open for paging (gba_memory.c:396-399).
- **Buffer scheme (all platforms including PSP):** at `retro_init` →
  `init_gamepak_buffer` (libretro.c:675) the core **greedily mallocs up to
  `ROM_BUFFER_SIZE` (=32) blocks of 1 MiB each, stopping silently at the
  first malloc failure** (gba_memory.c:2292-2303, blocksize at 378,
  gpsp_config.h:9-12). The psp1 target does **not** override
  `ROM_BUFFER_SIZE` (Makefile:242 — compare PS2 `-DROM_BUFFER_SIZE=16` at
  Makefile:451 and RS90 `=4` at Makefile:496), so on PSP it allocates *as
  many 1 MiB blocks as the heap allows*.
- If the ROM fits the allocated blocks, it is fully loaded at
  `load_gamepak_raw` (gba_memory.c:2686-2713). If not, the core **pages**:
  32 KiB pages loaded on demand from the open file into an LRU of
  1 MiB-block-backed 32 KiB slots (`load_gamepak_page` /
  `evict_gamepak_page`, gba_memory.c:2224-2290; LRU structures 380-391;
  `gamepak_must_swap` 2316-2324). Page faults go through the memory map
  (gba_memory.c:602, 1781-1793). Sticky bits pin pages per frame for the
  interpreter (gba_memory.h:314-327; cleared per frame only in the
  non-dynarec path, libretro.c:1413-1416).
- **Frontend gotcha (important):** because `init_gamepak_buffer` runs at
  `retro_init` and eats heap until malloc fails, the PSP frontend must either
  (a) pre-allocate its own buffers (GU display lists, audio ring, net
  buffers, UI) *before* calling `retro_init`, or (b) rebuild the core with
  `-DROM_BUFFER_SIZE=<n>` (it is a documented compile knob,
  gpsp_config.h:9-12) sized to leave headroom. Option (b) is a build-flag
  change, not a source patch.

### Dynarec/translation-cache on PSP

- psp1 defines `SMALL_TRANSLATION_CACHE` (Makefile:242) →
  `ROM_TRANSLATION_CACHE_SIZE = 2 MiB`, `RAM_TRANSLATION_CACHE_SIZE =
  384 KiB` (gpsp_config.h:14-21).
- On PSP these are **static `.bss` arrays**, not mmap: `MMAP_JIT_CACHE` is
  not set for psp1 (Makefile:5,237-249,553-555) and `mips_stub.S` places
  `rom_translation_cache`/`ram_translation_cache` in `.bss` for PSP
  (mips/mips_stub.S:614-631). So the 2.375 MiB is part of the EBOOT's BSS
  footprint, always resident.

### Paper memory budget — 16 MiB Pokémon ROM on PSP-1000

Static (linked into the EBOOT, always resident):

| Item | Size | Citation |
|---|---|---|
| ROM translation cache (.bss) | 2,048 KiB | gpsp_config.h:16; mips_stub.S:626-627 |
| RAM translation cache (.bss) | 384 KiB | gpsp_config.h:17; mips_stub.S:628-629 |
| `rom_branch_hash` (u32×65536) | 256 KiB | cpu_threaded.c:78; gpsp_config.h:28-29 |
| EWRAM (×2 SMC shadow) | 512 KiB | gba_memory.h:271 |
| IWRAM (×2 SMC shadow) | 64 KiB | gba_memory.h:272 |
| VRAM | 96 KiB | gba_memory.h:268 |
| `gamepak_backup` (SRAM/flash) | 128 KiB | gba_memory.c:360 |
| `sound_buffer` ring (s32×65536) | 256 KiB | sound.c:36; sound.h:23 |
| `memory_map_read` (8192 ptrs) | 32 KiB | gba_memory.h:274 |
| BIOS ×2 (`bios_rom` + builtin) | 32 KiB | gba_memory.h:263,269 |
| palette/OAM/IO/regs etc. | ~5 KiB | gba_memory.h:264-267,276 |
| **Static subtotal** | **≈3.72 MiB** | |

Heap (malloc at runtime):

| Item | Size | Citation |
|---|---|---|
| ROM buffer for 16 MiB ROM | 16,384 KiB (16 blocks; up to 32 MiB if heap allows) | gba_memory.c:2292-2303 |
| `gba_screen_pixels` | 75.5 KiB | libretro.c:682; common.h:113-116 |
| post-process buffers (only if cc/mix enabled) | 0–151 KiB | libretro.c:338-368 |
| `audio_sample_buffer` | ~4.3 KiB | libretro.c:604-608 |
| **Heap subtotal (16 MiB ROM, no post-fx)** | **≈16.1 MiB** | |

Frontend-side (ours, estimates — not core code): savestate staging buffer
416 KiB if used (savestate.h:99), GU draw buffers/display list + textures
(~0.5–1 MiB), audio out ring (tens of KiB), net driver (tens of KiB), UI.

**Verdict (paper):** core total ≈ **19.8 MiB** + EBOOT text/data (core .a is
a few MiB of code) + frontend buffers. A PSP-1000 gives homebrew a ~24 MiB
user partition (plus 4 MiB volatile via `sceKernelVolatileMemLock`) — those
partition numbers are PSP platform knowledge, **not** verifiable from this
repo; see Open questions. On paper a 16 MiB Pokémon ROM **fits fully
resident with roughly 3–4 MiB of headroom** for the frontend — adequate but
tight. Two safety levers, both build/config-only: `-DROM_BUFFER_SIZE=n`
(paging absorbs any shortfall at some ms0 I/O cost, gba_memory.c:2252-2290),
and allocating frontend memory before `retro_init` (§8 gotcha). Measure for
real at Gate 1 as planned.

## 9. Core options

Mechanism: v1 options API when `GET_CORE_OPTIONS_VERSION >= 1` — the core
sends `SET_CORE_OPTIONS_INTL` (libretro_core_options.h:429-446); legacy
`SET_VARIABLES` fallback otherwise (libretro_core_options.h:447-560). The
core reads values back with plain `GET_VARIABLE` and re-checks when
`GET_VARIABLE_UPDATE` says so (libretro.c:1438-1439). Full key list
(defaults **bold**; "load-only" keys are read only when
`check_variables(started_from_load=true)`, libretro.c:947-1013):

| Key | Values | Load-only? | Effect / relevance |
|---|---|---|---|
| `gpsp_bios` | **auto** / builtin / official | yes | BIOS selection; auto+official try `<system_dir>/gba_bios.bin`, validate first opcode byte 0x18 (libretro.c:948-958, 1199-1236). **We set `official`** per plan §3.4 |
| `gpsp_boot_mode` | **game** / bios | yes | Boot straight to game or via BIOS animation (libretro.c:960-968) |
| `gpsp_drc` | **enabled** / disabled | no (flushes caches on change) | Dynarec on/off (libretro.c:925-945). Keep enabled on PSP |
| `gpsp_rtc` | **auto** / enabled / disabled | yes | RTC force/auto (libretro.c:970-980); auto uses gba_over DB |
| `gpsp_serial` | **auto** / disabled / rfu / mul_poke / mul_aw1 / mul_aw2 | yes | Serial/RFU mode (libretro.c:982-1000 → serial.h:20-26). `auto` picks RFU for FR/LG/E via DB (gba_memory.c:1673-1677, 2977-2987). **Our wireless panel should pin `rfu`** (or trust auto for these ROMs) |
| `gpsp_rumble` | **auto** / enabled / disabled | yes | GPIO rumble (libretro.c:1002-1012) |
| `gpsp_sprlim` | **disabled** / enabled | no | "No sprite limit" — `enabled` removes the per-scanline limit (libretro.c:1015-1024; note inverted variable) |
| `gpsp_sound_rate` | 65536 / **32768 (ADR-0028: we now use this)** | changeable (triggers SET_SYSTEM_AV_INFO mid-game) | Mixer rate (libretro.c:1025-1031, 66-87). 32768 matches real GBA PWM bandwidth and halves mixing work — the "cheap PSP perf lever" was taken. Frontends must resample from `fe_host_sample_rate()`, never a literal. |
| `gpsp_frameskip` | **disabled** / auto / auto_threshold / fixed_interval | no | Frameskip mode (libretro.c:1034-1047). `auto*` need env #16 |
| `gpsp_frameskip_threshold` | 15..60, **33** | no | For auto_threshold (libretro.c:1049-1054) |
| `gpsp_frameskip_interval` | 0..10, **1** | no | For fixed_interval (libretro.c:1056-1062) |
| `gpsp_color_correction` | **disabled** / enabled | no | GBA-LCD color LUT post-process (libretro.c:1069-1078) — costs a full-frame pass; leave off on PSP |
| `gpsp_frame_mixing` | **disabled** / enabled | no | Interframe blend (libretro.c:1080-1089) — costs a pass + buffer swap; leave off on PSP |
| `gpsp_turbo_period` | 4..120, **4** | no | Turbo pulse period (libretro.c:1097-1117) |

(Option table: libretro_core_options.h:55-360.)

## 10. PSP-`#ifdef` and platform notes a frontend author must know

Complete list of `PSP`-conditional code in the core proper (grep over
`*.c,*.cc,*.h,*.S`):

1. **common.h:81-89** — under `#ifdef PSP` the core includes `<pspkernel.h>,
   <pspdebug.h>, <pspctrl.h>, <pspgu.h>, <pspaudio.h>, <pspaudiolib.h>,
   <psprtc.h>` and **relies on the PSP SDK's `u8/u16/u32/...` typedefs**
   instead of defining its own (common.h:90-99). Frontend code including
   `common.h` gets pspsdk types; harmless but be aware of collision if the
   frontend defines its own.
2. **cpu_threaded.c:227-231** — `platform_cache_sync` uses
   `sceKernelDcacheWritebackRange` + `sceKernelIcacheInvalidateRange`. The
   dynarec does its own I/D-cache maintenance; the frontend needs none for
   JIT, only for the *video* buffer it uploads to the GE (§2).
3. **mips/mips_stub.S:614-631** — translation caches live in `.bss` on PSP
   (no RWX section tricks needed; PSP has no memory protection). Confirms no
   mmap/JIT syscalls are required of the frontend.
4. **mips/mips_emit.h:2633-2636, 2658-2661** — Allegrex-specific codegen
   (`min` instruction; `cache 0x1A/0x08` ops for self-patching stubs).
   Frontend-invisible.
5. **mips/mips_codegen.h:282-294** — Allegrex `madd/maddu` encodings.
   Frontend-invisible.
6. **libretro-common/vfs/vfs_implementation.c:51-53,73-75,118-120** — the
   built-in VFS uses `<pspiofilemgr.h>`/`<pspkernel.h>`; returning false to
   `GET_VFS_INTERFACE` is fully supported on PSP.
7. **Threading assumptions: none.** The core is single-threaded; every
   callback (env, video, audio, input, netpacket receive/connected/…) is
   invoked from whichever thread calls `retro_run`/`retro_*`, and the
   netpacket send path is documented not-thread-safe (libretro.h:3106-3108).
   The plan's single-writer main-thread model (§4.3) matches exactly.
8. **Alignment:** no special alignment demands on frontend-provided data. The
   only aligned allocs in the core are 3DS-specific (libretro.c:342,680).
   `-G0` (Makefile:242) means no small-data section — frontend Makefile
   should match when linking.
9. **Netpacket specifics for our driver:** core sends everything
   `RELIABLE|FLUSH_HINT` (libretro.c:488-492); `poll` member is NULL
   (libretro.c:550) so the frontend's between-frame delivery plus the core's
   explicit `netpacket_poll_receive()` calls (libretro.c:483-486, invoked
   from serial code) are the only ingress points; `connected()` enforces a
   player cap = `MAX_RFU_NETPLAYERS`−1 = 31 clients for RFU
   (libretro.c:522-540; gpsp_config.h:31-32) — our 5-player cap is stricter
   and fine; `protocol_version` = `"gpSP v1.0"` (gpsp_config.h:7,
   libretro.c:553) — our session handshake must compare it.
10. **`retro_run` per-frame order** (libretro.c:1328-1440): input poll →
    frameskip decision → CPU (dynarec `execute_arm_translate(execute_cycles)`)
    → rumble → audio_run → video_run → serial/RFU frame update
    (`rfu_frame_update`, libretro.c:1429-1432) → variable-update check.
    Note RFU frame work happens *after* video; frontend pacing should call
    netpacket poll/flush after `retro_run` returns.

## Open questions

1. **PSP-1000 user-partition size (24 MiB user + 4 MiB volatile) is not
   derivable from this repo** — it is platform lore; the 32 MB fit verdict in
   §8 must be confirmed empirically at Gate 1 (heap probe log) as the plan
   already requires. The repo itself contains no PSP memory-partition code.
2. **EBOOT text/data size of the core** (`gpsp_libretro_psp1.a` linked) is
   unknown until we link the frontend; the §8 verdict assumes "a few MiB".
   Measure at first link.
3. **`retro_get_memory_size(SAVE_RAM)=128 KiB` vs `.sav` interop:** whether we
   trim `.sav` files to logical backup size (VBA/mGBA convention) or persist
   the full 128 KiB is a frontend policy decision — DECISIONS.md item, not a
   core constraint.
4. **`serial_proto.c` wire behavior** (message sizes/cadence, host-relay
   question) is out of scope here — covered by SERIAL-PROTO-NOTES.md (plan
   §3.3.1). This audit only confirms the libretro-glue side of netpacket.
5. `retro_reset` (`reset_gba`, libretro.c:835-838) during an active netpacket
   session: whether the RFU model survives a local reset cleanly was not
   auditable from the glue alone; test in Phase 4.

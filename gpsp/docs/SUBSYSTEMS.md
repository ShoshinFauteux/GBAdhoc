# SUBSYSTEMS — who owns what, and on which processor

The question this document answers is "I need to change X; what do I have to
understand, and what am I allowed to touch?"  `ARCHITECTURE.md` covers netdrv's
wire protocol in depth; this is the map above it.

Accurate as of 2026-09-17 on `opus/performance-stability-fixes`.  Where
something is inferred rather than verified, it says so.

---

## The picture

```
                        ms0: / ef0:   memory stick
                              |
      +-----------------------+------------------------+
      |                       |                        |
   ROM pages            SRAM / states          log, config, CMD.TXT
   (15 MB paged)        (io thread)            (io thread)
      |                       |                        |
      v                       v                        v
 +----------------------------------------------------------------+
 |                    MAIN CPU  (Allegrex, thread prio 0x2B)      |
 |                                                                |
 |  main_psp.c: init, main loop, ROM browser, lifecycle           |
 |    +-- ff_psp.c ............. frameskip policy + session pacing |
 |    +-- video_psp.c .......... framebuffer ring, GU blit, OSD    |
 |    +-- ui_psp.c ............. browser, menus, artwork           |
 |    +-- config_psp.c ......... config.ini load/save/migrate      |
 |    +-- mgift_cart.c ......... emulated distribution cart        |
 |    +-- mgift_net.c .......... infrastructure Wi-Fi + MGC2       |
 |    |                                                            |
 |  core (gpsp_libretro_psp1.a)                                    |
 |    +-- cpu_threaded.c ....... dynarec: translate, SMC, gates    |
 |    +-- gba_memory.c ......... memory map, paged ROM cache       |
 |    +-- video.cc ............. scanline renderer                 |
 |    +-- sound.c .............. GBA audio mixing                  |
 |    +-- rfu.c / serial*.c .... emulated wireless adapter         |
 +----------------------------------------------------------------+
        |              |                |              |
        | mailbox      | GE display     | sceAudio     | PDP / apctl
        v              v                v              v
 +--------------+  +--------+  +---------------+  +--------------+
 | MEDIA ENGINE |  |   GE   |  | audio thread  |  | adhoc rx/tx  |
 | 2nd core     |  | blit   |  | prio 0x12     |  | threads      |
 | video.cc on  |  | + OSD  |  |               |  | (netdrv)     |
 | the PRX      |  |        |  |               |  |              |
 +--------------+  +--------+  +---------------+  +--------------+
```

Other threads: `cb_thread` (prio 0x11) services the HOME/power callbacks;
`gpsp_io` (0x2C playable, 0x22 harness) is the only thread allowed to touch the
memory stick after boot; `gpsp_boost` guards the CPU-frequency window.

---

## Thread and processor ownership of shared state

The rule that matters: **a field written on one processor and read on another
needs an explicit handoff.**  Getting this wrong on the PSP does not fault, it
silently reads a stale cache line.

| state | written by | read by | handoff |
|---|---|---|---|
| `g_fb_off[]`, `g_fb_cur`, `g_draw_off` | main | main, GE | none needed; GE reads the address, not the variable |
| framebuffer pixels | main (blit) or ME | GE (display) | `sceGuSync` before `sceDisplaySetFrameBuf`; asserted by `tools/test_video_buffers.c` |
| `g_mer_*` staging buffers | ME (writes pixels) | main (presents) | ME writes **uncached**, so the GE sees them with no writeback |
| `me_capture_frame`, VRAM/OAM/palette snapshot | main (fills) | ME (reads) | ME invalidates its D-cache first (`me_inv`), then copies; `mb->input_seq` is published only after the copy, which is what lets the host resume emulating |
| VRAM dirty-page map | dynarec store stub + C write paths | ME | `u8[96]`, 1 = clean; `vram_clean == 0` means "no map, copy everything", set on the first frame and after any teardown |
| `g_audio_step` | ff_psp.c (pacing) | audio thread | `volatile`; a torn read costs one resample step, not correctness |
| EVT log ring | any thread (fe_evt) | `gpsp_io` only | `fe_evt_set_async`; exactly one consumer, enforced by contract not by a lock |
| SRAM / savestate files | `gpsp_io` only | — | ADR-0025: no other thread may touch the .sav |
| netdrv tx/rx rings | adhoc threads + main | both | SPSC rings, one producer and one consumer each |
| `g_pwr_park_req` / `g_pwr_parked` | `cb_thread` / main | main / `cb_thread` | `volatile` pair; the callback asks, the loop parks between frames and acknowledges |

---

## Contracts

### Display buffers (`psp/video_psp.c`)

Three framebuffers under `VID_TRIPLE`, indexed by `g_fb_cur`, addresses in
`g_fb_off[]`.  Two invariants:

1. **The GE must have finished before the LCD may latch.**  `vid_swap()` calls
   `vid_gu_flush()` (one `sceGuSync(0,0)`) before `sceDisplaySetFrameBuf`, and
   the host test asserts `g_gu_queued == 0` at that point.
2. **A buffer is only presentable once something drew into it.**  `g_fb_filled`
   is set by `vid_draw_frame`, `vid_draw_prestaged`, and by
   `vid_overlay_begin` *only when it clears*; `vid_swap()` returns early if it
   is unset.  This is the fast-forward black-bar fix: skipped frames used to
   rotate the ring anyway and present a buffer nobody had written.

`vid_blank_all()` clears `FB_BYTES * VID_NBUF` — it must track `VID_NBUF`, not
a hardcoded count.

**The fast-forward black-bar artifact is FIXED, for all three presets** (`3x`,
`Unlimited`, `Unlimited Smooth` -- `pcfg_ff_name`, psp/config_psp.c:32; the
former capped tiers were migrated away at config_psp.c:214).  It took two
changes, and both are needed:

* `ecdd8bc` added `g_fb_filled`, so an unfilled buffer is never presented and the
  ring does not rotate on a skipped frame.  `vid_blank_all()` was also fixed to
  clear `FB_BYTES * VID_NBUF` rather than a hardcoded two, which is what left
  buffer 2 holding stale VRAM.
* `add0f4d` fixed the other half, and it is the half that mattered for the
  unlimited presets: **three buffers are not a modulo-3 ring.** Fast-forward and
  pacing catch-up can submit several frames before one vblank, so
  `g_fb_filled` alone still allowed *drawing into the buffer the LCD was
  showing*.  `vid_swap()` now reads the live display address back after
  submitting and excludes both it and the submitted buffer, normalising VRAM
  aliases.  No added vblank wait on the normal path, and emulation is not capped
  to LCD refresh.

Confirmed on hardware by the user on a PSP-3000; see `FF-ARTIFACT-FIX.md` for
the full ownership rules and the host regression
(`tools/test_video_buffers.c`, 64,000 swap schedules).

**`gu_defer` is not a suspect and should not be A/B'd for this.**
`FF-ARTIFACT-FIX.md` rule 3 settled it from the pinned SDK's disassembly:
`sceGuSync(0, 0)` calls `sceGeDrawSync(0)`, so it is not a wait for just one of
the two GU lists.  `vid_swap()`'s pre-swap flush is also unconditional now,
deliberately, "so the invariant does not depend on gu_defer".

### ME capture and staging (`psp/me/me_render_glue.cc`)

Two phases per frame.  Phase 1 snapshots the core's VRAM (dirty pages only),
OAM and palette into the PRX's own arrays and then publishes `input_seq`, which
is what releases the host to emulate the next frame — so there is no torn read.
Phase 2 replays 160 scanlines through `update_scanline()` and writes pitched
rows to the host's staging buffer, uncached.

**The contract has a temporal seam, and it is load-bearing:** I/O registers are
captured **per scanline** (`cap->ioregs[ln]`, 160 rows), while **OAM, VRAM and
palette are one snapshot for the whole frame**, taken after the host finished
emulating it.  `reg[OAM_UPDATED]` is likewise a single value.

So a game that moves a sprite *during* the frame — a mid-frame OAM write, or an
HBlank DMA — is rendered by the ME with the sprite where it ended up, on every
line.  The CPU renderer, which runs scanline by scanline as emulation
progresses, sees OAM as it was at each line.  That is exactly the shape of
"a sprite in the wrong position for one frame", and heavy audio makes it more
likely because load changes *when* in the frame the game writes OAM.

This is independent of the dynarec: no path here can execute wrong code.  The
A/B that decides it is `me_mode = 0` (CPU renderer) versus the default.

### Dirty-page lifetime

`vram_clean` is a `u8[96]`, one byte per KB page, `1` = clean.  Producers are
the dynarec store stub and the C write paths (coverage verified at missed=0
over 255 marked pages).  `vram_clean == 0` (the null pointer, not a zeroed map)
means "no map available, copy all 96 KB", and the host sets that on the first
frame and after any teardown or resume, because the eDRAM mirror cannot be
trusted as a base to patch.

### ME renderer buffer ownership (`psp/main_psp.c`)

The engine's capture, staging and descriptor buffers come from the volatile
partition at `0x08400000` when `sceKernelVolatileMemLock` succeeds, and from the
heap otherwise.  Two callers need them released, for different reasons, and the
right policy is the opposite in each case:

| caller | why | volatile-backed | heap-backed |
| --- | --- | --- | --- |
| `me_rend_suspend()` | wireless `np_start` needs a clean heap (host run 407: passed at 0 KB held, failed at 41 KB) | keep rendering -- the engine holds no heap, so bring-up is unaffected | free everything |
| `me_standby_down()` | the OS owns the 4 MB pool and may re-hand it while we sleep | **must** release, and re-locking on wake is mandatory | already freed by teardown |

Getting the second one wrong is not a leak, it is a reset: the first standby
ladder left `g_mer_vmem` set, so resume saw "already locked", skipped the
re-lock, and rebuilt the renderer onto pointers into a partition we no longer
held.

**Nothing outside the renderer reads or writes `g_mer_*`.**  The lifecycle code
used to do both; it now goes through renderer-owned entry points:

* `me_rend_active()` -- is the engine drawing?
* `me_rend_vmem_held()` -- do we hold the volatile partition?  (logging only)
* `me_rend_present_src(&pitch)` -- the frame it would present now, or `NULL`
* `me_rend_release_vmem()` -- hand the partition back and drop every pointer
  into it; returns whether there was anything to release, because the standby
  ladder logs a rung only when it actually descends one
* `me_rend_suspend()` / `me_rend_resume()` -- the wireless pair

One ordering is load-bearing: `wake_snapshot()` runs **before**
`me_rend_teardown()`.  Teardown sets `g_mer_ready = -1`, so a snapshot taken
after it always found "nothing to copy" and the wake overlay came up on a flat
background instead of the game.

### SMC gates and selective invalidation (`cpu_threaded.c`)

Three invariants, and one that was tried and withdrawn.

1. **A translated block never contains a direct jump to an address the
   translator could not resolve.**  `block_lookup_translate_*` returns
   `BLOCK_LOOKUP_UNMAPPABLE` for a pc outside the regions it handles; patching
   that into a block emits `j 0x3FFFFFF`, i.e. a jump to `0x0FFFFFFC`, which is
   unmapped on a PSP — an instruction-fetch fault with no handler, and with no
   dispatcher call in the path, so `BADJUMP_SAFE` never sees it.

   Such an exit is instead patched **directly to `bios_swi_entrypoint`**, a
   permanent pre-generated block, and its `branch_source` is cleared so nothing
   patches it again (`cpu_threaded.c`, both eager loops).  That emits **no code
   at all** for the exit and so consumes no translation-cache bytes.

   An earlier version of this fix emitted a small per-exit dispatch island
   instead.  That was the measured performance regression — see the outcome
   section in [STABILITY-FIX-2026-09-17.md](STABILITY-FIX-2026-09-17.md).

   **This is containment, not correctness.**  It guarantees the console does not
   execute unmapped memory.  It does not establish that the guest behaves
   correctly afterwards: the block continues at the BIOS SWI entry rather than
   wherever the game intended.
2. **An unmappable guest pc never reaches `jr $v0`** (`BADJUMP_SAFE`): it
   soft-resets the GBA instead of the console.
3. **Learned SMC state may not outlive the ROM that produced it.**
   `smc_gates_reset()` clears the ranked candidate table — addresses, counts and
   the last value seen at each address — **at ROM load only** (`gba_memory.c`).
   Without it, a candidate saturated by one game promotes on a *single* write
   from the next, because the stale remembered value counts as a change.

   It is deliberately **not** called on savestate load, emulator reset, cheat
   install or config change, and that boundary is load-bearing. Earning a gate
   needs `SMC_GATE_MIN_SAMPLE` observations at one address, and
   `smc_gate_earned` runs once per SMC *flush event* — about 12 per 600 frames
   at `0300168c` in `heart_soul_light`, so roughly 3,200 frames to re-earn.
   Boot supplies those samples cheaply, and a savestate does not change ROM, so
   the candidates are still evidence about the right game.

   **Do not read the figures once quoted here as the cost of clearing at a
   savestate.**  The hardware A/B measured the arm that *added* those reset
   boundaries (`fix`) within noise of the arm without them (`dyn`) — +15.88% vs
   +16.35% on `heart_soul_light`, +8.13% vs +8.13% on `unbound_double_high`.
   The regression those numbers describe was caused by the **emitted dispatch
   islands**, which the same bisect isolated.  The extra reset boundaries were
   withdrawn because the evidence only ever justified ROM-load isolation, not
   because they were shown to be expensive.

**The cache-margin hazard is real, and invariant 1 no longer adds to it.**
`translation_cache_limit` is only tested inside the per-instruction emit loop, so
the gate epilogue and the `SMC_PARTIAL_SAFE` islands spend
`TRANSLATION_CACHE_LIMIT_THRESHOLD`'s margin unchecked — and the `ramtag` table
sits immediately after the RAM cache, so an overrun corrupts block metadata
instead of failing cleanly.

Invariant 1 contributes nothing to that pressure: patching to
`bios_swi_entrypoint` emits no bytes, so the containment fix consumes no margin
7283f13 did not already consume.  `UNMAPPABLE_ISLAND_BUDGET` was a reservation
added for the island version and was **removed with it**; it does not exist in
`cpu_threaded.c`.  Hardware A/B found the reservation made no measurable
difference either way (`nobudget` landed on `dyn`), so it is not the explanation
for anything.

**WITHDRAWN, 2026-09-18: two promotion-tightening rules.**  A staleness rule
(restart a candidate row after a ~60-frame gap) and a hardened writer fingerprint
(writer and target in IWRAM, one latched pc, N repetitions).  Both were
inferences with no demonstrated defect behind them, and each can silently switch
off something the accepted build relies on for speed: the staleness window is the
same order as the time to accumulate 64 writes at one address, so the hot gate
may never be earned; and the fingerprint gates selective invalidation, so an
assumption that does not hold costs a full flush on every mixer write.  Hardware
showed degraded performance and a wedge.  See the notes in `cpu_threaded.c`.

They are affordable to drop precisely because of invariant 1: a wrongly promoted
gate is now a dispatcher lookup, not a jump to `0x0FFFFFFC`.  The holes they
covered are asserted as holes in `tools/test_smc_safety.c` so nobody mistakes
them for covered — a role change inside one game can still promote on one write,
and a different writer pc with the same shape and the same `+0x3c` relationship
is still admitted.

Selective invalidation (`smc_writer_safe_range`) therefore admits exactly what
7283f13 admitted: an ARM `STMIA lr,{r0,r1}` whose highest written word sits
`0x3c` bytes past the writer.  **Anything uncertain keeps the full flush.**

Host proof: `tools/test_smc_safety.c`.

### RFU packet ownership and timing

The emulated adapter (`rfu.c`) and the link cable (`serial*.c`) belong to the
core.  The frontend never writes into them; it moves bytes through
`frontend-common/netpacket_host.c` and netdrv.  **Mystery Gift does not touch
this path at all** — `mgift_cart.c` speaks the game's protocol locally over
infrastructure Wi-Fi, and infrastructure mode and ad-hoc mode are mutually
exclusive, which is what keeps trading unaffected.

### Configuration migration (`psp/config_psp.c`)

`config.ini` is the user's file.  Unknown keys are preserved, missing keys take
defaults, and out-of-range values are corrected and counted by
`pcfg_validate()`.  A misspelled key fails silently and defaults — which has
cost this project two whole measurement campaigns — so verify a key against the
source before trusting a result that depends on it.

`.gpsp-harness.ini` is NOT a user file.  In a playable build ADR-0067 points it
at a path that cannot exist, so a leftover copy on a player's card is inert;
`tools/build.sh` asserts that neutralisation is compiled in.

---

## What the video oracle does and does not cover

`tools/e2e/run_video_regress.sh` is the project's only automated output-equivalence
gate, and it was silently passing everything from the ADR-0067 rename until
`d2857c1` (it wrote `autopilot.ini`, a channel that is read only so a leftover
can be reported and never obeyed, and never named a ROM).  It works now, and it
is still narrower than it looks.  **A PASS means "the CPU rendering path is
unchanged on this fixture".  It is not coverage of arbitrary changes.**

| path | covered? | why |
| --- | --- | --- |
| core software renderer (`video.cc`), CPU presentation | **yes**, 1091 frames | what the fixture exercises |
| ME renderer (`me_mode = 1`) | **no, and not fixable here** | PPSSPP loads the PRX but never runs the second core: `me_init reason=handshake magic=0x00000000`.  Only the hardware `me_mode` 0-vs-1 A/B can answer this -- and it has now been run: see [ME-RENDERER-DIVERGENCE.md](ME-RENDERER-DIVERGENCE.md), which found 188 frames the ME composed that the CPU renderer never produced, identical on both consoles |
| suspend / resume (`power_cb`, `wake_snapshot`, `me_standby_*`) | **no** | PPSSPP does not suspend; nothing in that path executes |
| same-frame retirement | **no** | the ini it writes omits `me_sameframe`, so `mer_sameframe_active()` is false |
| fast-forward pacing | partial | the boot leg is fast-forwarded, then `ff off` for the measured frames |

Before quoting a PASS as evidence for a change, check that the change is on a
covered row.  Three changes in this branch are on uncovered rows, and say so in
their commit messages rather than leaning on the PASS.

## Where the seams are not yet clean

Stated so the next person does not have to rediscover it.

* **`psp/main_psp.c` is 6387 lines** and still holds the ME renderer, the
  wireless session lifecycle, Mystery Gift orchestration, suspend/resume, the
  ROM browser and the main loop.
* **The ME renderer and the suspend/resume lifecycle are interleaved, but the
  interface between them is now closed.**  The region that looks like "the
  renderer" also contains `power_cb`, `standby_note`, `me_standby_up/down` and
  the wake snapshot.  The lifecycle half is 215 lines across six ranges, and it
  used to reach into renderer state directly: 10 renderer symbols inbound, 8 of
  them raw `g_mer_*` variables, 24 references.  It is now 6 symbols, **all
  functions, no variables**, 11 references (see the ownership contract above).
  Relocating it into `lifecycle_psp.c` is still a marginal trade and has NOT
  been done: 215 lines would move for ~12 crossing symbols, against the
  fast-forward move's 1052 lines for 6.  The encapsulation was the part worth
  having, and it stands on its own.
* **"INPUT SHADOWS" is a misleading header, kept because the code under it is
  load-bearing and well-explained.**  `g_mer_sh_vram/oam/pal` shadow the ME's
  *input arrays* -- VRAM, OAM and palette -- so the engine reads a stable copy
  instead of the host spinning 1415-1456 us per frame waiting for its own
  memcpy.  Nothing there touches controller input.  This cost one wrong reading
  of the boundary; if you are mapping this file, read the block comment, not the
  banner.
* **The ME renderer is 489 lines with only 5 symbols inbound -- the obstacle is
  now what reads it, not what it reads.**  Counting the renderer's own state
  declarations as part of it (they would travel with it, so counting them as
  coupling is wrong): 13 functions plus state, 489 lines, 5 inbound
  (`g_drew`, `g_ff_draw_all`, `g_ff_unlimited`, `g_fps_drawn`,
  `g_mer_rend_seen`) and 19 outbound, of which **10 are variables read directly
  from elsewhere in the file**.  31 external code sites, in three groups that
  need three different decisions:

  | group | sites | risk | verdict |
  | --- | --- | --- | --- |
  | ini/config binding (`me_mode`, `me_shadow`, `me_dirty`, `me_sameframe`, `vhash`) | 11 | none, boot-time only | a clean accessor/setter conversion, not yet done |
  | status and telemetry reads (`g_me_rend` in the FF mode check, the loop's status line) | 8 | none | same |
  | **frame-path presentation and stage ownership** | 12 | **on the frame path** | needs hardware measurement, NOT a mechanical conversion |

  The third group is the real work and the reason the renderer has not moved.
  It is the stage-ownership protocol between the main loop, the ME and the
  vcount-160 hook, and this project has already shipped one performance
  regression from a decomposition it could not explain.  Converting 12
  frame-path sites to function calls without a hardware A/B would repeat that.

* **Harness-only execution is not yet its own compilation unit, and the obvious
  first slice is not worth taking.**  `run_testpat` and `run_nettest` are only
  ~150 lines between them but need 8 symbols from `main_psp.c` -- including
  `plat_video_frame` and the Mystery Gift status strings `g_mg_l1/l2/linked/secs`
  -- so moving them would make 8 things public to relocate 150 lines.  Compare
  the fast-forward move: 1052 lines out for 6 inbound symbols.  The worthwhile
  version of this is the whole harness surface at once (the frame dumps, vhash,
  `vram_probe`, `vid_prof`, `perf_rig`, testpat and nettest), and that needs the
  same interface work as the renderer.
* `g_vc_target` (the vblank a frame is presented on) stayed in `main_psp.c`
  when pacing moved out, because the present loop owns it.  That is deliberate.
* `OUT_RATE` currently lives in `ff_psp.h`.  It belongs to audio and should
  travel with it when audio is extracted.

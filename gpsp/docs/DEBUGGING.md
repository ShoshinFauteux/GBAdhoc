# DEBUGGING — where to start, by symptom

Ordered by how often each symptom has actually happened on this project.
Every entry names the first thing to look at, not everything it could be.

Before anything else, two habits that have each cost this project a whole
campaign:

* **Run a known-good control first.** After ~40 launch/kill cycles PPSSPP stops
  initialising (32 MB RSS, no UI) and looks exactly like a broken build.
  If the control fails the same way, the rig is worn out, not the change.
* **Verify an ini key against the source before trusting a result.** A
  misspelled key fails silently and takes the default. Two measurement
  campaigns were lost to this.

---

## The console freezes hard (no crash screen, no exception)

A frozen PSP with no "Bad Execution Address" means execution left the
translation cache for somewhere unmapped, with nothing in the path that could
report it.

1. **Check you are not on a build older than the containment fix.** Before it,
   `translate_block_arm/thumb` patched `BLOCK_LOOKUP_UNMAPPABLE` — `(u8 *)(~0)`
   — straight into a live block as `j 0x3FFFFFF`, a jump to `0x0FFFFFFC`.
   `BADJUMP_SAFE` cannot catch that: no dispatcher call is involved.
2. **Build `diagnostic` and read `ms0:/smchisto.txt`.** The line
   `unmappable block exits contained: N` counts exits that *would* have been
   that jump. A rising N means the gate rule is promoting bad addresses.
3. **Read `ms0:/badjump.txt`** (diagnostic profile only). Each `*** RECOVERED`
   line is an unmappable guest pc that `BADJUMP_SAFE` turned into a GBA reset.
4. If N is high, the suspect is a gate on an address that is not an instruction
   boundary. `SMC_GATES_RANKED`'s promotion rule and its two lifetime rules are
   in `docs/SUBSYSTEMS.md`.

Do **not** reach for BADJUMP_REPORT in a build you intend to give anyone:
`gpsp_profile.h` refuses it in a release for a reason — it appends to the memory
stick from inside the dispatch path.

## A game boots to a white screen, or corrupts memory after a while

Almost always self-modifying code plus a gate or a partial flush.

1. Bisect with the flags, not the code: drop `SMC_GATES` first, then
   `SMC_PARTIAL_SAFE`. Either alone has been enough before.
2. `SMC_SKIP_SAME` is known to white-screen Heart & Soul at boot. It is kept
   for the record and refused in release builds.
3. `tools/test_smc_safety.c` is the fast check that the promotion rules still
   reject a data address and still accept the mixer's.

## Frame-time hitching, or fast-forward looks wrong

1. `python tools/run_host_tests.py --only video_buffers` — 64,000 swap
   schedules against the real `video_psp.c`. If that passes, the swap logic is
   not the problem.
2. Black bars in fast-forward were **fixed** in `ecdd8bc` + `add0f4d` and
   confirmed on a PSP-3000, for all three presets (`3x`, `Unlimited`,
   `Unlimited Smooth`). If they reappear it is a regression, and the two guards
   to check are `g_fb_filled` (never present an unfilled buffer) and
   `vid_swap()`'s read-back that excludes the live and pending display buffers —
   three buffers are *not* a modulo-3 ring, because FF can submit several frames
   per vblank. `docs/FF-ARTIFACT-FIX.md` has the ownership rules.
   **Do not A/B `gu_defer` for this**: rule 3 of that document rejected it from
   the SDK disassembly, and the pre-swap flush is unconditional by design.
3. Hitching on a large cart is usually ROM paging, not the dynarec: the OSD
   shows `pg N` (32 KiB stick reads per window) when the cart exceeds the cache.
4. Pacing decisions are all in `psp/ff_psp.c` now, and the harness knob
   `pace_slow_us` injects delay to prove the loop reacts.

## A sprite is in the wrong place for one frame

This is the ME renderer's temporal seam, not the dynarec. I/O registers are
captured per scanline; OAM, VRAM and palette are one snapshot per frame. A game
that moves a sprite mid-frame renders differently on the two paths.

The A/B is `me_mode = 0` (CPU renderer) versus the default. If the artifact
disappears with the CPU renderer, it is this and nothing else. See
`docs/SUBSYSTEMS.md` → "ME capture and staging".

## Wireless: a trade will not start, or drops

1. `docs/ADHOC-NOTES.md` is the reference; netdrv's wire format is in
   `docs/ARCHITECTURE.md`.
2. The receive wall (magic, version, type, exact length, CRC, src↔MAC) is
   load-bearing: the core's receive path is not garbage-tolerant. A frame that
   gets past it and should not have is a correctness bug, not a tuning one.
3. Mystery Gift shares none of this. It runs on infrastructure Wi-Fi, which
   cannot be up at the same time as ad-hoc, and that mutual exclusion is what
   keeps trading unaffected.

## Mystery Gift: the phone sends but the game never offers the menu

The discovery gate is one value: the game's Mystery Gift screen tests
`partner[idx].serialNo == RFU_SERIAL_WONDER_DISTRIBUTOR` (0x7F7D). Get it wrong
and the menu simply never lists the cart — no error anywhere.

On-screen diagnostics: the second OSD line shows
`<status> b<broadcasts> c<connects> cmd<last> st<state> S<saw_bcrd> C<saw_conn>`.
`b` rising with `c` stuck at 0 means the game **is** scanning and rejecting the
beacon — check the beacon checksum and field offsets, not the transport.

## A build "has no effect"

1. Did the flag reach the right make? Dynarec flags go to the **root** make;
   frontend defines go to `psp/Makefile`. This has produced two false-negative
   hardware experiments.
2. Did it rebuild? Neither makefile tracks define changes. `tools/build.sh`
   cleans by default; `--no-clean` does not.
3. Verify by **ELF md5**, never by EBOOT md5, and check the flags echoed in
   `psp/build-manifest.json`.
4. `PSP_EBOOT_TITLE` is ignored by the PBP tooling — titles are patched
   afterwards by `sfotitle.py`.

## Something links but dies instantly on hardware

`psp-fixup-imports` fails as a **warning** with make still exiting 0, which
leaves an EBOOT whose syscall imports are broken. It happens when
`psp/Makefile`'s `LIBS` names a library that psp-gcc's spec or `build.mak`'s
tail already links (`-lpspnet_inet` and `-lpspnet_apctl` are both implicit).
`tools/build.sh` greps for "stubs out of order" and fails the build.

## The emulator runs out of memory late in a session

gpSP mallocs until failure at ROM load, so the heap is already exhausted by
design. A frontend allocation made after that fails on a PSP-1000 only.
Anything that needs memory must reserve it at boot.

## The clock is wrong (1 Jan 2070 everywhere)

`time()` is roughly uptime on real firmware — correct in PPSSPP, wrong on
hardware. The wall clock is seeded from `sceRtcGetCurrentClockLocalTime`.

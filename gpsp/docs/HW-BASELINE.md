# Phase-0 hardware perf baseline — one-time checklist

Purpose (plan §5 Phase 0, risk register #1): confirm the **modern** gpsp core still runs Pokémon
at acceptable speed on real PSP hardware before we build a frontend on top of it. This is a single
transfer of stock, prebuilt software — none of our code is involved.

**Kit:** `hardware-baseline-kit/` next to the repo (RetroArch-PSP nightly 2026-07-30 + its prebuilt
gpsp core, already pruned to just what's needed).

## Steps (~10–15 min, one CFW PSP)

1. Copy your Emerald ROM (`.gba`) into the kit's `ROMS/` folder on your PC.
2. Connect the PSP by USB (USB mode). Copy the kit's `PSP` folder onto the memory stick root
   (it merges into `ms0:/PSP/GAME/`), and the `ROMS` folder to the memstick root too.
3. On the PSP: XMB → Game → Memory Stick → **RetroArch**. If your CFW blocks it, set the CFW's
   fake-firmware/plugin settings as you normally would for homebrew.
4. In RetroArch: Load Core → **gpSP**. Then Load Content → `ms0:/ROMS/` → your Emerald ROM.
   (No BIOS on the stick — the core will use its built-in open-source BIOS; that's fine for a
   perf test.)
5. Turn on the FPS counter: Menu → Settings → On-Screen Display → On-Screen Notifications →
   **Display Framerate** ON.
6. Play ~5 minutes: new game or loaded save — walk around a town (Littleroot/Petalburg), enter and
   exit a building, open the party menu, get into one wild battle.
7. Note, roughly:
   - FPS shown while walking in the overworld: ______
   - FPS in battle (including the battle intro animation): ______
   - Does it *feel* full speed (audio not crackling/slow-mo)? yes / no
   - Any crash/freeze? what were you doing: ______
8. (Optional, only if easy for you) Your CFW's VSH menu (select button in XMB, typically) has a
   **CPU CLOCK XMB/GAME** setting. If present: set GAME clock to 222, repeat step 6 for 2 minutes,
   note overworld FPS at 222: ______. Then set it back to default/333.
   (RetroArch-PSP requests 333 MHz itself; the CFW override is how we get a 222 datapoint. If your
   VSH menu doesn't offer it, skip — the 333 numbers are the ones that matter.)
9. Done. Report the numbers back in chat; they get recorded here and in DECISIONS.md.

## Notes

- Save files created during this test live in `ms0:/PSP/GAME/retroarch/savefiles/` — they won't
  touch anything else.
- If RetroArch fails to boot entirely on your CFW, report the CFW name/version + error instead;
  that is itself a useful datapoint (we may need a kernel-mode-friendly build or different nightly).
## RESULTS — measured 2026-08-01 (risk #1 CLOSED)

RetroArch-PSP would not launch on the user's ARK-4 6.61 CFW (both the nightly and the
1.22.2 stable PSP builds splash then return to the XMB), so the baseline was taken with
**our own EBOOT** instead — a better instrument anyway, since it measures the shipping
code path and reports numbers from the event log rather than an eyeballed overlay.
Method: `EVT heartbeat frames=N t_us=<wall clock µs>`, fps = 600e6 / Δt_us.

**Paced (normal) run — `EVT ff mode=normal`, `EVT clock=333`:**

- Per-600-frame samples: 58.5 – 59.0 fps, no outliers across 12,000 frames
- Sustained average: **58.6 fps against the GBA's 59.7275 target ≈ 98.2% of full speed**
- Frameskip was **disabled** for this measurement — i.e. this is the floor, not the ceiling
- User's subjective report: full speed; audio correct; brief hiccups only at scene
  transitions (battle intro, entering building interiors), full speed during battles

**Uncapped run — `EVT ff mode=uncapped`:**

- Range **67 fps (heavy intro scenes) → 137 fps (light scenes)** = 1.12× – 2.29× real time
- Average over the run: **84.9 fps ≈ 1.42× real time**, with every frame still rendered

**Verdict vs risk register #1 ("modern core underperforms on PSP"): CLOSED — the core
holds ~98% of full speed on real hardware with frameskip off.** Phase 5B is therefore a
polish pass, not a rescue mission. For fast-forward the raw ceiling is ~1.4× average with
rendering on; engaging the core's frameskip during FF (skipping presentation, keeping game
logic at pace) is what buys the advertised 2×+ — see the FF work in Phase 2.

Caveats for honesty: the two consoles are a PSP-1000 and a PSP-3000 and the *model used
for this baseline was not recorded* at the time; the 1000 was later observed to run
visibly laggier than the 3000 during wireless sessions (see HANDOFF issue #2), so treat
these figures as representative of a healthy unit rather than a guaranteed floor for the
1000-series. No 222 MHz comparison run was taken.

### Other real-hardware findings from the same session

- **Colors were wrong** (green→yellow): R/B channel order in the GU 16-bit blit. Our BMP
  dumps captured the *core's* framebuffer, upstream of the GU path, so every screenshot we
  had ever verified bypassed the broken code. Fixed in Phase 2 and now covered by
  `tools/e2e/run_gu_color_test.sh`, which reads back the actual GE drawbuffer and
  pixel-checks a test pattern.
- **Video was 1× centered** — expected for the Phase-1 build; scaling modes landed in
  Phase 2.
- **The frontend's graceful-failure path works on silicon:** with the WLAN switch off it
  logged `EVT net_error reason=wlan_off stage=wlan_switch` and exited cleanly
  (`exit code=4`) instead of crashing. (v1 had no UI to show a dialog, so it simply
  returned to the XMB — the Phase-2 UI shows a message instead.)

## Session perf — measured 2026-08-01 (the first field session that traded)

Two PSPs (PSP-3000 host, PSP-1000 client), real ad-hoc radio, Emerald<->Emerald Union
Room trade **completed**. fps below is computed from the `EVT heartbeat frames=N
t_us=…` ladder in each console's `frontend.log` (600 frames / Δt).

| | PSP-3000 (host) | PSP-1000 (client) |
|---|---|---|
| emulated fps, normal session windows | **58.96** | **56.5 – 57.6** |
| emulated fps, windows containing an `EVT sram_flush` | **56.49 / 55.20** | **50.65 / 50.38** |
| Δt of those windows | — | 11.84 s / 11.91 s vs ≈10.6 s normal |

Two things fall out of this, and they are the whole of the "the session feels laggy" report:

1. **Emulation was real-time the entire session** — 58.9 fps sustained on the host,
   identical to solo play. So `gpsp_frameskip=auto` (ADR-0018), which ran for the whole
   session, was discarding *rendered* frames with no throughput to show for it. The log
   could not reveal that, because the only frame number we emitted counted `retro_run`
   calls. `EVT fps emu=… rendered=… skipped=…` exists now for exactly this (ADR-0019),
   and a session no longer enables frameskip by default.
2. **Each 128 KiB SRAM flush froze the frame loop for roughly 0.5–1.3 s.** Only the
   flush windows are slow; their neighbours are normal. Fixed by writing only the 4 KiB
   blocks that changed (ADR-0020), and `EVT sram_flush … ms=` now reports the cost in
   every log so this never has to be inferred from heartbeat arithmetic again.

Radio numbers from the same session (see docs/TESTING.md for the full entry):
**`srtt_us` ≈ 52,000 typical (40,500 – 87,000)**, RTO ≈ 205 ms on spikes, `retx_pct` 2–3 %,
`overflow=0 spill=0 txfail=0`, core payload counters mirrored exactly — zero end-to-end loss.

The PSP-1000 does run measurably slower than the PSP-3000 (56.5–57.6 vs 58.96 fps) even in
its good windows, which is the one part of ADR-0018's premise that survives — hence the
opt-in `net_frameskip=1` adaptive mode rather than removing the mechanism outright.

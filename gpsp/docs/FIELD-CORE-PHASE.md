# Field `core_phase` measurements — real hardware, 2026-08-02

Recorded because these numbers were quoted to agents from a chat transcript and could not
be verified against anything in the repo. They come from the user's own consoles running
Build E (`bda7ae4e`, `core_phase=2`), pasted into the session and transcribed here
verbatim. All figures µs, `mean/max`, per 600-frame window.

Setup: PSP-1000 hosting, PSP-3000 joining, ad-hoc channel 1, `net_frameskip=1`,
`net_pace_match=1`, real Emerald with the user's save.

## PSP-3000, joining, steady in-session windows

```
tot=14253/25187 cpu=7754/15533 vid=3069/7096 blt=2973/3530 amix=313/544 aout=123/358
                jit=1/180 rfu=5/32 oth=12/79
tot=14152/24928 cpu=7674/14430 vid=3050/6948 blt=2977/3465 amix=310/556 aout=121/261
tot=13789/27332 cpu=7390/16818 vid=2974/6898 blt=2974/3506 amix=309/561 aout=120/369
```

## PSP-1000, hosting, steady in-session windows

```
tot=12239/18759 cpu=6071/8975  vid=2770/6356 blt=2945/3352 amix=307/635 aout=119/364
tot=12631/30963 cpu=7319/16029 vid=1853/4616 blt=2914/3440 amix=285/535 aout=121/351
tot=11857/38289 cpu=6824/23618 vid=1537/4948 blt=2846/3444 amix=254/539 aout=118/357
```

## Boot / early windows (before any peer connects)

```
tot=11219/20883 cpu=3961/13582 vid=4220/10367 blt=2557/3038  (PSP-1000)
tot=10294/20981 cpu=3763/13666 vid=3707/11496 blt=2523/3123  (PSP-3000)
```

## Readings

- **The frame is ~85 % full before our frontend does anything**: core `tot` ≈ 14.2 ms of a
  16.75 ms budget on the joining console.
- **Split on hardware: `cpu` ≈ 54 %, `vid` ≈ 22 %, `blt` ≈ 21 %, audio ≈ 3 %.** Note this
  differs from the rig (ADR-0032 measured video at 59 % of `retro_run`); the hardware
  `cpu` share is much larger. ADR-0032 predicted hardware would show video *higher*, not
  lower — that prediction did not survive contact with hardware, and the divergence is
  itself a finding. Treat rig ratios as a hypothesis about hardware, never a substitute.
- **`blt` ≈ 2.9-3.0 ms is OUR GU blit**, remarkably steady (max only ~3.5 ms). 21 % of every
  frame, in our own code — the largest lever we control.
- **The join seat costs ~+28 % `cpu`** (6071 → 7754 mean) versus hosting. `rfu` is 5 µs, so
  this is not our transport: it is the *game's own* RFU client code executing on the
  emulated CPU. Not optimisable by us.
- Worst frames are `cpu`-owned and coincide with flash-save bursts (`wbk` non-zero,
  `smc_addr win=4097 top=03007d90`), reaching `cpu:22953`/`cpu:23591`.
- These runs had `core_phase=2` active, which ADR-0032 prices at ~2.6 % overhead.

## Caveat for anyone quoting these

The mean `vid` figures above are *with* the level-2 probe active and are not probe-adjusted.
Do not compare them directly against rig numbers that have been probe-corrected without
saying which convention you are using — that ambiguity already caused one agent to suspect
a fabricated figure, correctly.

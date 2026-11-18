# video_emerald.hashes: provenance

`run_video_regress.sh` compares a run against this file. A golden nobody can
account for is not a gate, so this records what the file is and why it was
replaced.

## The current golden (2026-10-11)

- 1091 frames, md5 see `git log`. Captured in PPSSPP with
  `VREGRESS_EBOOT=<a local lab build>/EBOOT.PBP` (md5 dbbf9770, harness
  profile, `GBA_LINK=1`, source c6c0462 = the code of `claude/rc-4.0.0`
  c8b0b36, the base of the 4.0.0 beta with ~170 clean hardware fixture runs),
  `testdata/fixtures/emerald_vregress.inputs`, Emerald (USA, Europe) rev 0,
  `Pokemon Emerald All Shiny Fixed.sav` (md5 e5af0862), official BIOS
  (md5 a860e8c0).
- Reproduced three times on 2026-10-11: that EBOOT, the stage-1 harness
  (1d0472b3) and the stage-2 harness (bcfb55ae) give the same 1091 lines.
- So a PASS means: every frame of the workload is pixel-identical to what the
  rc-4.0.0 base renders. It does not mean the base is right.

## Why the 2026-08-02 golden (dd16ca4, 1090 frames) was replaced

It failed on every build since the oracle was repaired (d2857c1, 2026-09-18,
which already noted "1090 frames against 1091"); everyone since compared
build against build with `VREGRESS_GOLDEN` and the tracked file gated
nothing. Two causes, found 2026-10-11:

1. **Channel order (proven).** 594837a (ADR-0039, 2026-08-03, the day after
   the capture) made the core emit the GE's 5650 order instead of libretro
   RGB565: red and blue trade places in every pixel of the hashed buffer.
   Only frames of pure black/white kept their hash; the first coloured frame
   (11) and everything after differed. Proof: the harness key
   `vhash_legacy_rb = 1` hashes each pixel with red and blue exchanged; with
   it, frames 1-367 of today's build equal the old golden exactly
   (`VREGRESS_INI_EXTRA="vhash_legacy_rb = 1"`).
2. **One frame of lag at the Start menu (cause not bisected).** With the
   channel order undone, today's run has ONE extra copy of frame 367 (the
   overworld just before the Start menu opens, after `press START 4`), i.e.
   the game answers that press one frame later than on 2026-08-02; that is
   the 1091st frame. From there the two runs line up again shifted by one,
   identical wherever the picture is static and different only while
   something animates (golden frames 424-467, 552-820, 1003-1090: the menu
   and party-screen transitions), as a one-frame phase shift between the
   script's inputs and the game's free-running animations gives. No picture
   content was found to differ beyond that phase.
   The change lies between 2026-08-02 and 2026-09-18, the weeks the oracle
   was a no-op (the ADR-0036 control-file rename, often miscited as ADR-0067), so nothing saw it happen. Which commit
   (emulation timing or the frame an autopilot press reaches the core) is
   NOT known: bisecting needs the runner patched at every step of a window
   in which it could not run. It predates v2.x and is in every release since.

3. **Line endings (found 2026-10-11).** `.gitattributes` had `* text=auto`, so a
   Windows checkout of this file has CRLF and the runner's `diff` against a
   run captured in WSL (LF) fails on every line even when every hash is
   equal. `tools/e2e/goldens/*.hashes` is now `eol=lf`.

## Rules

- Recapture only with the reason written here, and from a build whose
  identity (md5, commit) is recorded.
- A change that is meant to alter pixels compares build against build
  (`VREGRESS_GOLDEN`), then recaptures.

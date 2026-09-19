# Reporting a GBAdhoc bug

Copy the block below into the report and fill it in. The first five lines are
the ones that decide whether a problem can be reproduced at all — a report
without them usually cannot be acted on, however clear the description is.

```
GBAdhoc version:        (the version shown on the ROM browser, e.g. v1.1.0-7283f13)
PSP model:              1000 / 2000 / 3000 / Go / other
Firmware:               (System Settings -> System Information)
Game and revision:      (e.g. Pokemon FireRed (USA) rev 1, or the ROM hack and its version)
Fast-forward profile:   off / 1.5x / 3x / uncapped / uncapped smooth

Settings changed from the defaults:
                        (paste the lines you edited in GBADHOC/CONFIG.INI, or "none")

Wireless role, if relevant:
                        host / join / Mystery Gift / not wireless

What happened:

What you expected instead:

Exact steps to reproduce:
  1.
  2.
  3.

Does it survive a restart of GBAdhoc?      yes / no / not tried
Does it survive a restart of the PSP?      yes / no / not tried
Does it happen on a fresh save?            yes / no / not tried
```

## Attachments that help

* **A photo or short video** for anything visual — flickering, tearing, black
  bars, a sprite in the wrong place. A still frame is often enough, and
  describing a display artifact in words almost never is.
* **A savestate or save file**, when the problem needs a specific point in the
  game and the file is yours to share. Do not attach ROMs or BIOS images.
* **`GBADHOC/CONFIG.INI`**, if you changed anything in it.

## What not to send

* ROM files, BIOS images, or anything else that is not yours to distribute.
* `GBADHOC/log/` from an ordinary build — a release build does not write one.
  If you were asked for a log, you were given a diagnostic build and told which
  file to send.

## If the console froze completely

Say so explicitly, and say what you were doing in the ~30 seconds before it —
particularly whether you had:

* played a different game earlier in the same GBAdhoc session, without returning
  to the browser;
* changed the in-game music or soundtrack setting;
* toggled fast-forward repeatedly;
* entered or left a battle.

A hard freeze is rare and usually needs a specific sequence, so that
recollection is worth more than any single detail about the moment it happened.

## For developers triaging this

`docs/DEBUGGING.md` maps symptoms to the first thing to look at.
`docs/SUBSYSTEMS.md` says which subsystem owns what, so a report can be routed
without reading the whole frontend.

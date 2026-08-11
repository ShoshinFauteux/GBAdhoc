# VARIANTS.md — per-game "invisible" builds

User product direction (2026-08-01, personal builds): a build-time recipe
that turns gpSP-AdHoc into what looks and feels like a native PSP port of
one specific GBA game — its own XMB icon and title, boots straight into
the game, no emulator UI ever required, and wireless "just works" when the
game itself uses its wireless features.  The generic build (XMB title
**"PSP AGB"**, icon/PIC1 from `psp/assets/`, ROM browser + full UI) is
unaffected.

## Building a variant

```
tools/make_variant.sh <romfile.gba> <icon0.png|-> \
    [--title "Name"] [--group GPSPnn] [--role auto|host|join] [--out DIR]
```

Produces `dist/<name>/` containing `EBOOT.PBP` (custom `PSP_EBOOT_TITLE` +
`ICON0`, packed by the pspdev docker image; the canonical EBOOT is
repacked with the default "PSP AGB" branding afterwards), the ROM,
`variant.ini`, and a user-facing `README.txt`.  Install = copy the folder
to `ms0:/PSP/GAME/<anything>/` on each PSP.  Prereq: the core archive
(`make platform=psp1`) and frontend objects are built.

## Runtime behavior (`variant.ini` next to the EBOOT)

| Key | Meaning |
|---|---|
| `rom = <file>` | ROM to auto-boot, relative to the install dir. Browser is never shown. |
| `silent_wireless = 1` | Enable the ADR-0013 silent session policy (below). |
| `role = auto\|host\|join` | Pin the netdrv role, or `auto` (default): join-first, promote-to-host on timeout. |
| `group = GPSPnn` | Fixed adhocctl group for this game (default `GPSP07`). |

All paths are derived from the EBOOT's directory at runtime (`argv[0]`),
so any install folder name works and multiple variants coexist.  Saves
live in a **private namespace**: `<install>/saves/<rom>.sav` (plus
`.sav.bak` session backups and `.st0` savestates) — a variant never
touches another install's saves.  `config.ini`/`.gpsp-harness.ini` are also
per-install.  **`variant.ini` is deliberately user-facing and unchanged;
only the harness channel was renamed (ADR-0036).**

## Silent wireless (ADR-0013 summary)

The core fires `gpsp_rfu_activated_hook()` (weak symbol, 4-line core
patch in `rfu.c`) when a game completes the RFU adapter login handshake —
i.e. the exact moment the player used an in-game wireless feature.  The
frontend then, with zero UI:

1. Backs up the save, brings up the adhoc transport on the fixed group.
2. `role=auto`: starts netdrv as **join** and waits a jittered window
   (240 + 0..119 frames, clock-derived) for a host's WELCOME; if none
   answers, restarts the netpacket layer as **host** (contention backoff:
   simultaneous activations pick different promote times, so one side
   hosts first and the other's JOIN retry latches on).  `role=host/join`
   skips the negotiation.
3. Feedback is OSD-only: "Wireless linked"/"Wireless ready" toasts and
   the session chip.  WLAN switch off ⇒ persistent overlay warning
   ("Turn the WLAN switch ON…"), logged `EVT silent_fail reason=wlan_off`,
   no retry storm.

The policy is validated end-to-end by `tools/e2e/run_silent_trade.sh`:
two PPSSPP instances with `silent=1` (autopilot mirror of the variant
keys), inst1 role-auto promotes itself (`EVT silent_host promoted=1`),
inst2 pinned-join latches on (`EVT silent_joined`), and the full Gate-4E
Union Room trade + save oracle must hold over the silently-negotiated
session.

Known limits (v1): FF stays interlocked off while the session is up (the
session persists until app exit); if two auto-role sides promote inside
the same jitter window they host separate sessions and the game simply
finds no partner — re-entering the wireless counter retries.  Pin roles
(or distinct groups per pair) for tournament-style setups.

# Automatic hotspot, shortcut and gift arrival

The user confirmed successful Mystery Gift delivery on hardware with Gift Fix 1.
The remaining Automatic-profile issue and presentation requests are addressed
by **GBAdhoc Gift Polish** (`ecdd8bc-mgift2`) and app `0.1.0-mgift2` (version 3).

## Automatic

Automatic means SSID **Mystery Gift**, open security, DHCP, automatic DNS, no
proxy. **GBAdhoc Mystery Gift** is only a saved PSP profile's display name.

The old path tried to recreate an existing profile, prepared it before network
initialization and ignored SetNetParam failures. The new path initializes
APCTL first, searches all ten slots for matching settings, and borrows a match
without changing it. Otherwise it creates a free slot, checks every write and
verifies the copied profile before connecting. Existing mismatched/secured
profiles are preserved. A failed new profile is cleaned up before network
teardown; a successful profile stays for reuse. Manual selection still works.

SDK semantics are documented in the [PSPSDK network-parameter API](https://pspdev.github.io/pspsdk/psputility__netparam_8h.html):
Create clears scratch slot 0, Set writes slot 0, and Copy takes source then
destination. Hardware testing still needs to confirm the fresh-profile and
reuse cases; the exact SCE failure in the original hardware run was not logged.

## Select + Down

Outside the emulator menus, **Select + Down** toggles the Mystery Gift listener
using the selected profile (Automatic by default). Hold the chord to toggle
once; release both buttons before toggling again. Both buttons are consumed
until release so the chord cannot scroll the game menu. Other shortcut buttons,
scripted input and the wake overlay exclude this shortcut. Existing ad-hoc
sessions keep their radio interlock and receive the existing toast instead.

## App presentation

The broadcast sweep remains indeterminate. A PSP acknowledgement starts a
roughly five-second visual arrival: a Wonder Card follows a curved path with
sparkles, shrinks into the illustrated PSP and gives way to the ready screen.
The progress bar advances steadily during this presentation. No synthetic
byte percentage or game-save confirmation is shown. The actual UDP protocol
and delivery time are unchanged; **Ready in emulator** still tells the player
to receive and save the card through the game's own menu.

The UI resets on retry/failure and remembers arrival progress across ordinary
recomposition. Android's disabled-animation preference may shorten animations.
DS/3DS presentation paths retain their existing network progress behavior.

## Verification

- `tools/test_mgift_profiles.c` includes the production network code and shortcut
  helper. SDK stubs check init-before-profile access, matching-profile reuse,
  manual override, free-slot creation, secured/mismatched profiles, full slots,
  setter/copy/readback failure cleanup, and chord debounce/input consumption.
- Existing Mystery Gift parcel/cart, FF scheduling/config and 64,000 buffer-swap
  tests remain green. The protocol reaching the game is unchanged from the
  hardware-validated Gift Fix 1.
- Android unit tests and release compilation pass. The release APK continues
  to use the repository's existing development signing key.
- Android emulator visual checks cover search, arrival, landing and ready in
  portrait and landscape. The landscape layout keeps the progress bar visible.
  The PSP information card now shows hotspot settings and the shortcut rather
  than the obsolete RFU/336-byte description. The temporary preview activity
  was excluded from the release and retained only in the local audit folder.
- Build/source hashes, test logs and device installation manifests are in
  `builds/mgift-polish/` in the containing workspace.

Hardware follow-up: enable with the shortcut from the game's Mystery Gift
screen, receive a card, disable, then enable again with Automatic. Test a PSP
with no matching saved profile as well as one with the existing named profile.
Verify Select+Start and save/load shortcuts remain distinct. Confirm the phone's
search/arrival/ready animation and cancel/retry in portrait and landscape.

The public release ZIP, README rewrite, separate app repository and promotional
demonstrations remain deferred until the user finishes validation.

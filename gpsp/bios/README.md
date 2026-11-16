This BIOS is an open source replacement for Nintendo's official BIOS.
It was written originally by Normmatt and the VBA/VBA-M team, and its source
code can be found at https://github.com/Nebuleon/ReGBA/tree/master/bios

It is distributed under the GPL2 license (see repo)

Sound driver (GBAdhoc, 2026-11)
-------------------------------
The image upstream ships answers the BIOS sound-driver calls (SWI 19h-1Eh,
28h, 29h) with a bare return.  Games that leave PCM mixing to the BIOS
(X-Men: Reign of Apocalypse calls SoundDriverInit/Mode/Main/VSync) then never
switch the sound hardware on and run silent.  source/sound_driver.c
implements those calls; the music-player calls (20h-24h) and the sequencer
jump table are still empty returns.

The rest of open_gba_bios.bin is kept byte for byte (it was built with
devkitARM long ago).  splice_sound_driver.sh compiles sound_driver.c alone
(ARMv4T, ARM state, freestanding; arm-linux-gnueabihf-gcc in docker), links
it at 0x2800 (sound_driver.ld), copies it into the zero tail of the image and
points the eight SWI table entries at it.  Re-run it after editing
sound_driver.c.  source/core.s names the same functions, so a full devkitARM
build of this directory gives an equivalent image (different addresses).

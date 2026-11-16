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
sound_driver.c (it leaves every byte below 0x2800 alone, so the word patches
below survive).  source/core.s names the same functions, so a full devkitARM
build of this directory gives an equivalent image (different addresses).

Local changes to open_gba_bios.bin (GBAdhoc)
--------------------------------------------

The build environment has no devkitARM, so these are made to the shipped
binary word by word, with the same change in source/softwareinterrupts.c:

* 2026-11 (KarnFu, issue #9, Mortal Kombat: Deadly Alliance): the header
  loads of RLUnCompWram (0x0f18), Diff8bitUnFilterWram (0x10c4),
  Diff8bitUnFilterVram (0x1124) and Diff16bitUnFilter (0x11b0) were
  post-indexed LDRs ("ldr rT,[rB],#4"); each is now "ldmia rB!,{rT}", the
  same register and writeback.  Nintendo's BIOS reads these headers with a
  THUMB LDMIA: the word at (src & ~3), not rotated, with the stream at
  src + 4.  An LDR of a halfword-aligned src rotates the word, so the length
  came out as anything from 0 to megabytes (MK:DA's Midway logo unpacked
  ~5.5 MB over EWRAM: a 50 s freeze, then broken title and menu graphics).
  RLUnCompVram already aligned its source; LZ77 and Huffman are left alone
  (Nintendo's BIOS reads those headers with an ARM LDR as well).

Image hashes (md5): upstream 1876f71b0d8c65eef3454547896ffb11; with the
sound driver 13802ca70661a573d56e3a61d819a368; with the sound driver and the
header-read patch (the shipped image) 523d85d4ba25ab4b8460604174d5dd80.

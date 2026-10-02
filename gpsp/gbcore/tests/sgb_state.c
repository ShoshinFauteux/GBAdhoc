/* A state carries its DMG/SGB model (gbcore_state_load).
 *
 * On FE_CONSOLE_GB the DMG palette setting picks the hardware model at
 * power-on: Auto boots an SGB-aware cart as a Super Game Boy, any explicit
 * palette boots it as a DMG.  A state saved under one model must still load
 * after the palette changed -- the 3.1 RC state shelf quit to the XMB when
 * it did not.  The loaded machine runs on, and re-saving it records the
 * saved model, not the one the palette chose at boot. */
#include "../gbcore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void on_video(void *u, const gbcore_video_frame_t *f) { (void)u; (void)f; }
static void on_audio(void *u, const int16_t *s, size_t n) { (void)u; (void)s; (void)n; }

static void make_rom(unsigned char rom[0x8000], int sgb)
{
  static const unsigned char logo[48] = {
    0xCE,0xED,0x66,0x66,0xCC,0x0D,0x00,0x0B,0x03,0x73,0x00,0x83,
    0x00,0x0C,0x00,0x0D,0x00,0x08,0x11,0x1F,0x88,0x89,0x00,0x0E,
    0xDC,0xCC,0x6E,0xE6,0xDD,0xDD,0xD9,0x99,0xBB,0xBB,0x67,0x63,
    0x6E,0x0E,0xEC,0xCC,0xDD,0xDC,0x99,0x9F,0xBB,0xB9,0x33,0x3E
  };
  unsigned i, sum = 0;
  unsigned char check = 0;
  memset(rom, 0, 0x8000);
  rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 0x01; /* JP $0150 */
  memcpy(rom + 0x104, logo, sizeof(logo));
  memcpy(rom + 0x134, "SGB STATE", 9);
  rom[0x146] = sgb ? 0x03 : 0x00;          /* SGB support flag */
  rom[0x14B] = 0x33;                       /* new licensee: required by SGB */
  for (i = 0x134; i <= 0x14C; i++) check = (unsigned char)(check - rom[i] - 1);
  rom[0x14D] = check;
  /* LD SP,$FFFE; LD A,$91; LDH ($40),A; then count in WRAM forever:
   * INC (HL) with HL=$C000, JR -3 -- a state taken mid-run has a moving
   * byte that a later frame must keep moving. */
  rom[0x150] = 0x31; rom[0x151] = 0xFE; rom[0x152] = 0xFF;
  rom[0x153] = 0x3E; rom[0x154] = 0x91;
  rom[0x155] = 0xE0; rom[0x156] = 0x40;
  rom[0x157] = 0x21; rom[0x158] = 0x00; rom[0x159] = 0xC0; /* LD HL,$C000 */
  rom[0x15A] = 0x34;                                         /* INC (HL) */
  rom[0x15B] = 0x18; rom[0x15C] = 0xFD;                      /* JR -3 */
  for (i = 0; i < 0x8000; i++) if (i != 0x14E && i != 0x14F) sum += rom[i];
  rom[0x14E] = (unsigned char)(sum >> 8);
  rom[0x14F] = (unsigned char)sum;
}

static gbcore_t *boot(unsigned char *rom, unsigned palette)
{
  gbcore_callbacks_t cb;
  memset(&cb, 0, sizeof(cb));
  cb.video = on_video;
  cb.audio_batch = on_audio;
  gbcore_set_palette(palette);
  return gbcore_create(NULL, rom, 0x8000, FE_CONSOLE_GB, 32768, &cb);
}

static int run(gbcore_t *c, unsigned frames)
{
  while (frames--)
    if (gbcore_run_frame(c, 0) != 0)
      return -1;
  return 0;
}

/* Header byte 7 of a GBAT state is the model it holds (1 DMG, 2 SGB). */
static int save(gbcore_t *c, unsigned char *buf, size_t cap, long *len)
{
  *len = gbcore_state_save(c, buf, cap);
  return *len > 0 ? buf[7] : -1;
}

static int fail(const char *what)
{
  fprintf(stderr, "FAIL sgb_state: %s\n", what);
  return 1;
}

/* Save under `from`'s palette, reboot under `to`'s, load. */
static int cross(int sgb_cart, unsigned from, unsigned to, int want_saved,
                 int want_boot)
{
  static unsigned char rom[0x8000], st[256 * 1024], st2[256 * 1024];
  long len = 0, len2 = 0;
  uint8_t before, after;
  gbcore_t *c;
  make_rom(rom, sgb_cart);

  c = boot(rom, from);
  if (!c || run(c, 30) != 0) return fail("boot (save side)");
  if (save(c, st, sizeof(st), &len) != want_saved) return fail("saved model");
  gbcore_shutdown(c);

  c = boot(rom, to);
  if (!c || run(c, 5) != 0) return fail("boot (load side)");
  if (save(c, st2, sizeof(st2), &len2) != want_boot) return fail("boot model");
  if (gbcore_state_load(c, st, (size_t)len) != 0) return fail("load refused");
  /* The machine now IS the saved one: same model, same bytes. */
  if (save(c, st2, sizeof(st2), &len2) != want_saved || len2 != len)
    return fail("model after load");
  if (gbcore_peek(c, 0xC000, &before, 1) != 0 || run(c, 3) != 0 ||
      gbcore_peek(c, 0xC000, &after, 1) != 0 || before == after)
    return fail("machine does not run on after the load");
  gbcore_shutdown(c);
  printf("sgb_state cart=%s palette %u->%u model %d->%d: loaded\n",
         sgb_cart ? "SGB" : "DMG", from, to, want_saved, want_boot);
  return 0;
}

int main(void)
{
  static unsigned char rom[0x8000], st[256 * 1024];
  long len = 0;
  gbcore_t *c;

  /* SGB cart: Auto (SGB) <-> explicit palette (DMG), both ways. */
  if (cross(1, GBCORE_PALETTE_AUTO, GBCORE_PALETTE_GREEN, 2, 1) ||
      cross(1, GBCORE_PALETTE_GREEN, GBCORE_PALETTE_AUTO, 1, 2) ||
      cross(1, GBCORE_PALETTE_GREY, GBCORE_PALETTE_POCKET, 1, 1) ||
      cross(0, GBCORE_PALETTE_AUTO, GBCORE_PALETTE_GREEN, 1, 1))
    return 1;

  /* Still refused: a header that disagrees with the block, a short image,
   * another cartridge's state (the SGB-flag byte is part of the identity,
   * so an SGB state never reaches a cart without SGB support). */
  make_rom(rom, 1);
  c = boot(rom, GBCORE_PALETTE_AUTO);
  if (!c || run(c, 10) != 0 || save(c, st, sizeof(st), &len) != 2)
    return fail("sgb save");
  gbcore_shutdown(c);
  make_rom(rom, 0);
  c = boot(rom, GBCORE_PALETTE_AUTO);
  if (!c) return fail("dmg boot");
  if (gbcore_state_load(c, st, (size_t)len) == 0)
    return fail("SGB cart's state accepted by a DMG-only cart");
  gbcore_shutdown(c);

  make_rom(rom, 1);
  c = boot(rom, GBCORE_PALETTE_GREEN);
  if (!c) return fail("boot");
  st[7] = 1;                               /* header says DMG, block says SGB */
  if (gbcore_state_load(c, st, (size_t)len) == 0)
    return fail("inconsistent model accepted");
  st[7] = 2;
  if (gbcore_state_load(c, st, (size_t)len - 1) == 0)
    return fail("short state accepted");
  if (gbcore_state_load(c, st, (size_t)len) != 0)
    return fail("good state refused after rejected ones");
  gbcore_shutdown(c);
  printf("PASS sgb_state\n");
  return 0;
}

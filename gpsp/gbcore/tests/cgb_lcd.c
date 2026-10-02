/* The Game Boy Color LCD model (gbcore_set_color_correction,
 * docs/GB-PALETTE-FIXES.md).
 *
 * A generated CGB cartridge loads one BG palette -- white, mid grey
 * (16,16,16), pure blue (0,0,31) and a red-orange (31,8,0) -- and fills the
 * screen with a tile showing colours 0,0,1,1,2,2,3,3.  The picture must be:
 *
 *   - with the model on (the default): each entry through the LCD curve,
 *     green mixed with blue (_cgb_lcd_table.h);
 *   - with it off: the plain 5-to-8-bit expansion (3.0.0's picture);
 *   - switched at run time: the next frame, without the game rewriting
 *     its palette;
 *
 * and a DMG cartridge's picture does not depend on the setting.
 */
#include "../gbcore.h"
#include "../tgbdual/_cgb_lcd_table.h"

#include <stdio.h>
#include <string.h>

static uint16_t px[8];
static unsigned frames;

static void on_video(void *u, const gbcore_video_frame_t *f)
{
  (void)u;
  memcpy(px, f->pixels, sizeof(px));   /* row 0, pixels 0..7 */
  frames++;
}

static void on_audio(void *u, const int16_t *s, size_t n)
{
  (void)u; (void)s; (void)n;
}

/* Frontend layout of the host build: libretro RGB565, red high. */
static uint16_t rgb(unsigned r, unsigned g, unsigned b)
{
  return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static unsigned expand(unsigned c5) { return (c5 << 3) | (c5 >> 2); }

static uint16_t raw(uint16_t c)
{
  return rgb(expand(c & 31), expand((c >> 5) & 31), expand((c >> 10) & 31));
}

static uint16_t lcd(uint16_t c)
{
  unsigned r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
  return rgb(_cgb_lcd_curve[r], _cgb_lcd_green[g * 32 + b], _cgb_lcd_curve[b]);
}

static const uint16_t pal[4] = { 0x7FFF, 0x4210, 0x7C00, 0x011F };

static size_t make_rom(unsigned char *rom, int color)
{
  static const unsigned char logo[48] = {
    0xCE,0xED,0x66,0x66,0xCC,0x0D,0x00,0x0B,0x03,0x73,0x00,0x83,
    0x00,0x0C,0x00,0x0D,0x00,0x08,0x11,0x1F,0x88,0x89,0x00,0x0E,
    0xDC,0xCC,0x6E,0xE6,0xDD,0xDD,0xD9,0x99,0xBB,0xBB,0x67,0x63,
    0x6E,0x0E,0xEC,0xCC,0xDD,0xDC,0x99,0x9F,0xBB,0xB9,0x33,0x3E
  };
  unsigned char *p = rom + 0x150;
  unsigned i, sum = 0;
  unsigned char check = 0;
  memset(rom, 0, 0x8000);
  rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 0x01;   /* JP $0150 */
  memcpy(rom + 0x104, logo, sizeof(logo));
  memcpy(rom + 0x134, "CGBLCDTEST", 10);
  rom[0x143] = color ? 0x80 : 0x00;
  rom[0x14A] = 0x01;
  for (i = 0x134; i <= 0x14C; i++) check = (unsigned char)(check - rom[i] - 1);
  rom[0x14D] = check;

  *p++ = 0x31; *p++ = 0xFE; *p++ = 0xFF;          /* LD SP,$FFFE */
  *p++ = 0x21; *p++ = 0x00; *p++ = 0x80;          /* LD HL,$8000 */
  *p++ = 0x06; *p++ = 0x08;                       /* LD B,8 */
  *p++ = 0x3E; *p++ = 0x33;                       /* loop: LD A,$33 */
  *p++ = 0x22;                                    /* LD (HL+),A */
  *p++ = 0x3E; *p++ = 0x0F;                       /* LD A,$0F */
  *p++ = 0x22;                                    /* LD (HL+),A */
  *p++ = 0x05;                                    /* DEC B */
  *p++ = 0x20; *p++ = 0xF7;                       /* JR NZ,loop */
  *p++ = 0x3E; *p++ = 0x80;                       /* BCPS: index 0, auto */
  *p++ = 0xE0; *p++ = 0x68;
  for (i = 0; i < 4; i++)                         /* BCPD x 8 */
  {
    *p++ = 0x3E; *p++ = (unsigned char)(pal[i] & 0xFF);
    *p++ = 0xE0; *p++ = 0x69;
    *p++ = 0x3E; *p++ = (unsigned char)(pal[i] >> 8);
    *p++ = 0xE0; *p++ = 0x69;
  }
  *p++ = 0x3E; *p++ = 0x91;                       /* LCD on, BG on, $8000 */
  *p++ = 0xE0; *p++ = 0x40;
  *p++ = 0x18; *p++ = 0xFE;                       /* JR -2 */

  for (i = 0; i < 0x8000; i++)
    if (i != 0x14E && i != 0x14F) sum += rom[i];
  rom[0x14E] = (unsigned char)(sum >> 8);
  rom[0x14F] = (unsigned char)sum;
  return 0x8000;
}

static int expect(const char *what, uint16_t (*conv)(uint16_t))
{
  static const int idx[8] = { 0, 0, 1, 1, 2, 2, 3, 3 };
  int i;
  for (i = 0; i < 8; i++)
  {
    uint16_t want = conv(pal[idx[i]]);
    if (px[i] != want)
    {
      fprintf(stderr, "FAIL cgb_lcd %s: pixel %d = %04x, want %04x\n", what, i,
              px[i], want);
      return 1;
    }
  }
  return 0;
}

static int run(gbcore_t *core, unsigned n)
{
  while (n--)
    if (gbcore_run_frame(core, 0) != 0)
      return 1;
  return 0;
}

int main(void)
{
  static unsigned char rom[0x8000];
  gbcore_callbacks_t cb;
  gbcore_t *core;
  uint16_t dmg_on[8], dmg_off[8];
  int bad = 0;

  memset(&cb, 0, sizeof(cb));
  cb.video = on_video;
  cb.audio_batch = on_audio;

  if (gbcore_color_correction() != 1)
  {
    fprintf(stderr, "FAIL cgb_lcd: the model is not on by default\n");
    return 1;
  }
  make_rom(rom, 1);
  core = gbcore_create(NULL, rom, sizeof(rom), FE_CONSOLE_GBC, 32768, &cb);
  if (!core || run(core, 10))
    return 1;
  bad |= expect("default", lcd);
  gbcore_set_color_correction(0);
  if (run(core, 1))
    return 1;
  bad |= expect("off", raw);
  gbcore_set_color_correction(1);
  if (run(core, 1))
    return 1;
  bad |= expect("on again", lcd);
  gbcore_shutdown(core);

  /* DMG: the DMG palette is the frontend's choice, not CGB colour. */
  make_rom(rom, 0);
  core = gbcore_create(NULL, rom, sizeof(rom), FE_CONSOLE_GB, 32768, &cb);
  if (!core || run(core, 10))
    return 1;
  memcpy(dmg_on, px, sizeof(px));
  gbcore_set_color_correction(0);
  if (run(core, 1))
    return 1;
  memcpy(dmg_off, px, sizeof(px));
  gbcore_set_color_correction(1);
  gbcore_shutdown(core);
  if (memcmp(dmg_on, dmg_off, sizeof(px)) != 0)
  {
    fprintf(stderr, "FAIL cgb_lcd: a DMG picture changed with the setting\n");
    bad = 1;
  }
  if (!bad)
    printf("PASS cgb_lcd: GBC LCD model on by default, off = raw, switched "
           "live; DMG unaffected (%u frames)\n", frames);
  return bad;
}

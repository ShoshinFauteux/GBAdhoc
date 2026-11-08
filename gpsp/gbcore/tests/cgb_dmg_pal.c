/* The Game Boy Color's colours for DMG games (issue #34,
 * gbcore/tgbdual/_cgb_dmg_pal.h, GBCORE_PALETTE_GBC_*).
 *
 *  1. Selection, against known cartridge headers (titles and licensee
 *     bytes only; no ROMs): the title sum, the combination the GBC boot ROM
 *     picks, the 4th-letter disambiguation (Metroid II and Super Mario Land
 *     share a sum), the licensee rule (only Nintendo titles are coloured;
 *     old code 0x01 or 0x33 + "01"), and the fallback (combination 0).
 *  2. The table: every combination and manual palette resolves inside the
 *     colour array (run under AddressSanitizer), the manual palettes are
 *     the documented ones.
 *  3. The core: a generated DMG cartridge titled TETRIS shows combination 3
 *     through the GBC LCD model (raw with the model off), switched live;
 *     the four-shade palettes are unchanged; a CGB game ignores the
 *     setting; and the machine is identical whatever the palette -- same
 *     save state and sync hash after the same frames, also for an
 *     SGB-flagged cartridge (the GBC palettes boot it as a DMG, as GREY
 *     does).  GBCORE_PALETTE_WIRE sends them to a link peer as GREY.
 */
#include "../gbcore.h"
#include "../tgbdual/_cgb_lcd_table.h"
#include "../tgbdual/_cgb_dmg_pal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int bad;

#define CHECK(cond, ...) do { if (!(cond)) { \
  fprintf(stderr, "FAIL cgb_dmg_pal: " __VA_ARGS__); fputc('\n', stderr); \
  bad = 1; } } while (0)

/* ------------------------------------------------------------ selection */

enum { LIC_OLD_NINTENDO, LIC_NEW_NINTENDO, LIC_OTHER, LIC_NEW_OTHER };

static void make_header(unsigned char hdr[0x1C], const char *title, int lic)
{
  memset(hdr, 0, 0x1C);
  memcpy(hdr, title, strlen(title) > 16 ? 16 : strlen(title));
  switch (lic)
  {
  case LIC_OLD_NINTENDO: hdr[0x14B - 0x134] = 0x01; break;
  case LIC_NEW_NINTENDO: hdr[0x14B - 0x134] = 0x33;
                         hdr[0x144 - 0x134] = '0';
                         hdr[0x145 - 0x134] = '1'; break;
  case LIC_OTHER:        hdr[0x14B - 0x134] = 0x08; break;   /* Capcom */
  case LIC_NEW_OTHER:    hdr[0x14B - 0x134] = 0x33;
                         hdr[0x144 - 0x134] = '0';
                         hdr[0x145 - 0x134] = '8'; break;
  }
}

/* Title sums are hand-checked constants, and the expected
 * combinations come from the assigned-palette list (tcrf.net, Notes:Game
 * Boy Color Bootstrap ROM), cross-checked with SameBoy's boot ROM data. */
static const struct {
  const char *title;
  int lic;
  unsigned sum, combo;
} known[] = {
  { "TETRIS",           LIC_OLD_NINTENDO, 0xDB,  3 },
  { "ALLEY WAY",        LIC_OLD_NINTENDO, 0x88,  4 },
  { "DR.MARIO",         LIC_OLD_NINTENDO, 0x3C, 15 },
  { "ZELDA",            LIC_OLD_NINTENDO, 0x70, 44 },
  { "MARIOLAND2",       LIC_OLD_NINTENDO, 0xC9, 37 },
  { "KIRBY DREAM LAND", LIC_OLD_NINTENDO, 0x49, 36 },
  { "SUPERMARIOLAND3",  LIC_OLD_NINTENDO, 0x59, 32 },
  { "POKEMON RED",      LIC_NEW_NINTENDO, 0x14, 13 },
  { "POKEMON GREEN",    LIC_NEW_NINTENDO, 0xAA, 14 },
  /* 4th letter decides */
  { "POKEMON BLUE",     LIC_NEW_NINTENDO, 0x61, 11 },
  { "KIRBY2",           LIC_NEW_NINTENDO, 0xB3, 36 },
  { "METROID2",         LIC_OLD_NINTENDO, 0x46, 46 },   /* MET[R]OID2 */
  { "SUPER MARIOLAND",  LIC_OLD_NINTENDO, 0x46, 22 },   /* SUP[E]R... */
  { "KID ICARUS",       LIC_OLD_NINTENDO, 0xBF, 24 },
  { "TETRIS2",          LIC_OLD_NINTENDO, 0x0D, 31 },
  { "DONKEYKONGLAND",   LIC_OLD_NINTENDO, 0x18, 39 },
  { "DONKEYKONGLAND 2", LIC_OLD_NINTENDO, 0x6A, 39 },
  /* a known sum whose 4th letter matches no entry: the default */
  { "METXOID2",         LIC_OLD_NINTENDO, 0x4C,  0 },
  { "MEXROID2",         LIC_OLD_NINTENDO, 0x4A,  0 },
  /* same sum as METROID2 (0x46), wrong 4th letter */
  { "METAOIDC",         LIC_OLD_NINTENDO, 0x46,  0 },
  /* not Nintendo: never looked up */
  { "TETRIS",           LIC_OTHER,        0xDB,  0 },
  { "POKEMON RED",      LIC_NEW_OTHER,    0x14,  0 },
  /* Nintendo, but not in the table */
  { "NOT IN TABLE",     LIC_OLD_NINTENDO, 0x30,  0 },
};

static void test_selection(void)
{
  unsigned char hdr[0x1C];
  size_t i;
  for (i = 0; i < sizeof(known) / sizeof(known[0]); i++)
  {
    unsigned sum, combo;
    make_header(hdr, known[i].title, known[i].lic);
    sum = tgbshared_cgbpal_title_sum(hdr);
    combo = tgbshared_cgbpal_auto(hdr);
    CHECK(sum == known[i].sum, "%s: title sum %02X, want %02X",
          known[i].title, sum, known[i].sum);
    CHECK(combo == known[i].combo, "%s (licensee case %d): combination %u, "
          "want %u", known[i].title, known[i].lic, combo, known[i].combo);
  }
  /* The whole 16-byte title counts, not just up to the first NUL. */
  make_header(hdr, "TETRIS", LIC_OLD_NINTENDO);
  hdr[15] = 0x80;
  CHECK(tgbshared_cgbpal_title_sum(hdr) == 0x5B, "16th title byte ignored");
}

/* ---------------------------------------------------------------- table */

static void colors(unsigned combo, unsigned short out[12])
{
  tgbshared_cgbpal_colors(combo, out);
}

static void test_table(void)
{
  static const unsigned short p0[4]  = { 0x7FFF, 0x32BF, 0x00D0, 0x0000 };
  static const unsigned short p4[4]  = { 0x7FFF, 0x421F, 0x1CF2, 0x0000 };
  static const unsigned short p24[4] = { 0x7FFF, 0x03FF, 0x001F, 0x0000 };
  static const unsigned short p25[4] = { 0x03FF, 0x001F, 0x000C, 0x0000 };
  static const unsigned short p28[4] = { 0x7FFF, 0x7E8C, 0x7C00, 0x0000 };
  static const unsigned short p29[4] = { 0x7FFF, 0x1BEF, 0x6180, 0x0000 };
  unsigned short c[12], ref[12];
  unsigned i;
  for (i = 0; i < TGB_CGBPAL_COMBOS + 3; i++)   /* past the end: combo 0 */
    colors(i, c);
  colors(TGB_CGBPAL_COMBOS, c);
  colors(0, ref);
  CHECK(!memcmp(c, ref, sizeof(c)), "out-of-range combination not 0");

  /* Combination 0 (the default): BG 29, OBJ0 and OBJ1 4. */
  CHECK(!memcmp(ref, p29, 8) && !memcmp(ref + 4, p4, 8) &&
        !memcmp(ref + 8, p4, 8), "combination 0 is not BG 29 / OBJ 4,4");
  /* Tetris (3): 24 everywhere.  Pokemon Red (13): BG 4, OBJ0 3, OBJ1 4.
   * Pokemon Blue (11): BG 28, OBJ0 4, OBJ1 28.  Metroid II (46): BG 28,
   * OBJ0 25, OBJ1 3. */
  colors(3, c);
  CHECK(!memcmp(c, p24, 8) && !memcmp(c + 4, p24, 8) &&
        !memcmp(c + 8, p24, 8), "Tetris colours");
  colors(13, c);
  CHECK(!memcmp(c, p4, 8) && !memcmp(c + 8, p4, 8), "Pokemon Red colours");
  colors(11, c);
  CHECK(!memcmp(c, p28, 8) && !memcmp(c + 4, p4, 8) &&
        !memcmp(c + 8, p28, 8), "Pokemon Blue colours");
  colors(46, c);
  CHECK(!memcmp(c, p28, 8) && !memcmp(c + 4, p25, 8), "Metroid II colours");
  /* A combination that starts a palette one colour early (22: OBJ at
   * colour 15 = palette 3's last colour, then palette 4's first three). */
  colors(22, c);
  CHECK(c[4] == 0x0000 && c[5] == 0x7FFF && c[6] == 0x421F &&
        c[7] == 0x1CF2, "combination 22 OBJ0 offset");

  /* Spot checks of the manual palettes by colour. */
  colors(tgbshared_cgbpal_manual(TGB_CGBPAL_UP), c);
  CHECK(!memcmp(c, p0, 8) && !memcmp(c + 4, p0, 8) && !memcmp(c + 8, p0, 8),
        "Up is not palette 0 everywhere");
  colors(tgbshared_cgbpal_manual(TGB_CGBPAL_RIGHT_A), c);
  CHECK(!memcmp(c, ref, sizeof(c)), "Right+A is not combination 0");
  colors(tgbshared_cgbpal_manual(TGB_CGBPAL_DOWN_A), c);
  CHECK(!memcmp(c, p24, 8), "Down+A BG is not palette 24");
  colors(tgbshared_cgbpal_manual(TGB_CGBPAL_LEFT), c);
  CHECK(!memcmp(c, p28, 8) && !memcmp(c + 4, p4, 8), "Left colours");
  CHECK(tgbshared_cgbpal_manual(TGB_CGBPAL_MANUAL_COUNT) == 0,
        "out-of-range manual palette not 0");
}

/* ----------------------------------------------------------------- core */

static uint16_t px[8];

static void on_video(void *u, const gbcore_video_frame_t *f)
{
  (void)u;
  memcpy(px, f->pixels, sizeof(px));   /* row 0, pixels 0..7 */
}

static void on_audio(void *u, const int16_t *s, size_t n)
{
  (void)u; (void)s; (void)n;
}

/* Host frontend layout: RGB565, red high. */
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

static const uint16_t cgb_pal[4] = { 0x7FFF, 0x4210, 0x7C00, 0x011F };

/* A tile with colours 0,0,1,1,2,2,3,3 over the whole screen, BGP = $E4.
 * `color`: CGB-only cartridge loading cgb_pal; `sgb`: SGB flag set. */
static void make_rom(unsigned char *rom, const char *title, int color, int sgb)
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
  memcpy(rom + 0x134, title, strlen(title));
  rom[0x143] = color ? 0x80 : 0x00;
  rom[0x146] = sgb ? 0x03 : 0x00;
  rom[0x14A] = 0x01;
  rom[0x14B] = sgb ? 0x33 : 0x01;          /* SGB needs the new code */
  rom[0x144] = '0'; rom[0x145] = '1';
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
  *p++ = 0x3E; *p++ = 0xE4;                       /* BGP = $E4 */
  *p++ = 0xE0; *p++ = 0x47;
  *p++ = 0x3E; *p++ = 0x80;                       /* BCPS: index 0, auto */
  *p++ = 0xE0; *p++ = 0x68;
  for (i = 0; i < 4; i++)                         /* BCPD x 8 */
  {
    *p++ = 0x3E; *p++ = (unsigned char)(cgb_pal[i] & 0xFF);
    *p++ = 0xE0; *p++ = 0x69;
    *p++ = 0x3E; *p++ = (unsigned char)(cgb_pal[i] >> 8);
    *p++ = 0xE0; *p++ = 0x69;
  }
  *p++ = 0x3E; *p++ = 0x91;                       /* LCD on, BG on, $8000 */
  *p++ = 0xE0; *p++ = 0x40;
  /* A counter in WRAM, so the state moves every frame. */
  *p++ = 0x21; *p++ = 0x00; *p++ = 0xC0;          /* LD HL,$C000 */
  *p++ = 0x34;                                    /* loop: INC (HL) */
  *p++ = 0x18; *p++ = 0xFD;                       /* JR loop */

  for (i = 0; i < 0x8000; i++)
    if (i != 0x14E && i != 0x14F) sum += rom[i];
  rom[0x14E] = (unsigned char)(sum >> 8);
  rom[0x14F] = (unsigned char)sum;
}

/* A fixed wall clock: a state carries the clock's stamp, and runs that
 * straddle a second must still compare equal. */
static time_t fixed_clock(void) { return (time_t)1700000000; }

static gbcore_t *boot(const unsigned char *rom, fe_console_t console,
                      unsigned palette)
{
  gbcore_callbacks_t cb;
  memset(&cb, 0, sizeof(cb));
  cb.video = on_video;
  cb.audio_batch = on_audio;
  if (gbcore_power_on() != 0)
    return NULL;
  gbcore_set_wallclock(fixed_clock);     /* power_on cleared it */
  gbcore_set_palette(palette);
  return gbcore_create(NULL, rom, 0x8000, console, 32768, &cb);
}

static int run(gbcore_t *core, unsigned n)
{
  while (n--)
    if (gbcore_run_frame(core, 0) != 0)
      return 1;
  return 0;
}

static void expect_px(const char *what, const uint16_t want4[4])
{
  static const int idx[8] = { 0, 0, 1, 1, 2, 2, 3, 3 };
  int i;
  for (i = 0; i < 8; i++)
    CHECK(px[i] == want4[idx[i]], "%s: pixel %d = %04x, want %04x", what, i,
          px[i], want4[idx[i]]);
}

/* State bytes and sync hash of `rom` after 30 frames with `palette`. */
static int machine(const unsigned char *rom, fe_console_t console,
                   unsigned palette, unsigned char **state, long *len,
                   uint64_t *hash)
{
  gbcore_t *core = boot(rom, console, palette);
  size_t cap;
  if (!core || run(core, 30))
    return 1;
  cap = gbcore_state_size(core);
  *state = (unsigned char *)malloc(cap);
  *len = *state ? gbcore_state_save(core, *state, cap) : -1;
  *hash = gbcore_sync_hash(core);
  gbcore_shutdown(core);
  return *len <= 0;
}

static void test_core(void)
{
  static unsigned char rom[0x8000];
  unsigned short tetris[12];
  uint16_t want[4];
  gbcore_t *core;
  unsigned i;

  tgbshared_cgbpal_colors(3, tetris);

  /* DMG game, GBC (auto): Tetris colours through the GBC LCD model. */
  make_rom(rom, "TETRIS", 0, 0);
  core = boot(rom, FE_CONSOLE_GB, GBCORE_PALETTE_GBC_AUTO);
  CHECK(core && !run(core, 10), "Tetris boot");
  if (!core)
    return;
  for (i = 0; i < 4; i++) want[i] = lcd(tetris[i]);
  expect_px("GBC (auto), model on", want);
  gbcore_set_color_correction(0);
  CHECK(!run(core, 1), "frame");
  for (i = 0; i < 4; i++) want[i] = raw(tetris[i]);
  expect_px("GBC (auto), model off", want);
  gbcore_set_color_correction(1);
  /* Switched live to Grey: the four-shade palette, as before. */
  gbcore_set_palette(GBCORE_PALETTE_GREY);
  CHECK(!run(core, 1), "frame");
  want[0] = raw(0x7FFF);                          /* FFFFFF */
  want[1] = raw(21 | 21 << 5 | 21 << 10);         /* A8A8A8 */
  want[2] = raw(11 | 11 << 5 | 11 << 10);         /* 585858 */
  want[3] = raw(0);
  expect_px("Grey after GBC", want);
  /* ...and to a manual palette (Up: palette 0). */
  gbcore_set_palette(GBCORE_PALETTE_GBC_UP);
  CHECK(!run(core, 1), "frame");
  {
    unsigned short up[12];
    tgbshared_cgbpal_colors(tgbshared_cgbpal_manual(TGB_CGBPAL_UP), up);
    for (i = 0; i < 4; i++) want[i] = lcd(up[i]);
    expect_px("GBC Up", want);
  }
  gbcore_shutdown(core);

  /* The same DMG game on FE_CONSOLE_GBC (a .gb renamed .gbc): also. */
  core = boot(rom, FE_CONSOLE_GBC, GBCORE_PALETTE_GBC_AUTO);
  CHECK(core && !run(core, 10), "Tetris on GBC boot");
  if (core)
  {
    for (i = 0; i < 4; i++) want[i] = lcd(tetris[i]);
    expect_px("GBC (auto) on FE_CONSOLE_GBC", want);
    gbcore_shutdown(core);
  }

  /* A CGB game ignores the DMG palette. */
  make_rom(rom, "TETRIS", 1, 0);
  core = boot(rom, FE_CONSOLE_GBC, GBCORE_PALETTE_GBC_AUTO);
  CHECK(core && !run(core, 10), "CGB boot");
  if (core)
  {
    for (i = 0; i < 4; i++) want[i] = lcd(cgb_pal[i]);
    expect_px("CGB game with GBC (auto)", want);
    gbcore_shutdown(core);
  }

  /* The machine does not depend on the palette: same state and hash for
   * GREY and every GBC palette, for a plain and an SGB-flagged DMG cart
   * (which GREY and the GBC palettes both boot as a DMG). */
  {
    static const struct { const char *title; int sgb; } carts[] = {
      { "METROID2", 0 }, { "POKEMON RED", 1 },
    };
    unsigned c;
    for (c = 0; c < 2; c++)
    {
      unsigned char *ref = NULL, *got = NULL;
      long ref_len = 0, got_len = 0;
      uint64_t ref_hash = 0, got_hash = 0;
      unsigned pal;
      make_rom(rom, carts[c].title, 0, carts[c].sgb);
      CHECK(!machine(rom, FE_CONSOLE_GB, GBCORE_PALETTE_GREY, &ref, &ref_len,
                     &ref_hash), "%s grey run", carts[c].title);
      for (pal = GBCORE_PALETTE_GBC_AUTO; pal < GBCORE_PALETTE_COUNT; pal++)
      {
        CHECK(!machine(rom, FE_CONSOLE_GB, pal, &got, &got_len, &got_hash),
              "%s palette %u run", carts[c].title, pal);
        CHECK(got_len == ref_len && ref && got &&
              !memcmp(got, ref, (size_t)ref_len) && got_hash == ref_hash,
              "%s: machine differs with palette %u (%s)", carts[c].title,
              pal, gbcore_palette_name(pal));
        free(got);
        got = NULL;
      }
      free(ref);
    }
  }

  /* Link wire value: GBC palettes as GREY, the others unchanged. */
  for (i = 0; i < GBCORE_PALETTE_COUNT + 2; i++)
  {
    unsigned w = GBCORE_PALETTE_WIRE(i);
    unsigned want_w = i >= GBCORE_PALETTE_GBC_AUTO && i < GBCORE_PALETTE_COUNT
                      ? GBCORE_PALETTE_GREY : i;
    CHECK(w == want_w, "wire value of %u is %u", i, w);
  }
  CHECK(!strcmp(gbcore_palette_name(GBCORE_PALETTE_GBC_AUTO), "GBC (auto)"),
        "name of GBC_AUTO");
  CHECK(!strcmp(gbcore_palette_name(GBCORE_PALETTE_GBC_RIGHT_B),
                "GBC Right+B"), "name of GBC_RIGHT_B");
}

int main(void)
{
  test_selection();
  test_table();
  test_core();
  if (!bad)
    printf("PASS cgb_dmg_pal: %u known headers, 51 combinations + 12 manual, "
           "GBC LCD model, CGB untouched, machine palette-independent\n",
           (unsigned)(sizeof(known) / sizeof(known[0])));
  return bad;
}

/* test_me_wmap.c -- the ME mid-frame log's palette/OAM write maps
 * (cpu.h OAM_WMAP/PAL_WMAP, video_me_log.h "write maps").
 *
 * The capture compares only the 32-byte rows the maps name, so a store that
 * changes a row without marking it would silently drop that change from the
 * log.  This drives the PRODUCTION C store paths (write_memory8/16/32) and
 * dma_transfer (every direction, both widths, spans that wrap the 1 KiB
 * mirror and spans that cross into the next 16 MiB region) and checks, after
 * every operation, that the map covers every row whose bytes changed (a
 * missing row is a lost log entry) -- and for a DMA that stays in one region
 * EXACTLY those rows (an extra row is wasted work); a C store marks all rows
 * and a DMA that crosses a 16 MiB boundary may.
 * me_wmap_rows() is checked against a brute-force model as well.
 *
 * The MIPS store stubs are not reachable here; the qemu twin's log digest
 * (tools/drprof/dr_host.c DR_MLOG) covers them.
 *
 * tools/tests/run_me_wmap_tests.sh builds this once as is (must pass) and
 * once per ME_WMAP_MUTANT (gba_memory.c / gba_memory.h; each must fail). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../gba_memory.c"

u32 cpu_ticks;
u32 frame_counter;
u32 execute_cycles;
u32 reg[64];
u32 cph_dman;
u32 cph_bkr;
u32 cph_bkw;
u32 sound_frequency = 1;
u32 sound_freq_bits = 0;
u32 sound_on;
timer_type timer[4];
direct_sound_struct direct_sound_channel[2];
gbc_sound_struct gbc_sound_channel[4];
u32 gbc_sound_master_volume_left;
u32 gbc_sound_master_volume_right;
u32 gbc_sound_master_volume;
u32 gbc_sound_buffer_index;
u32 gbc_sound_last_cpu_ticks;
u16 palette_ram[512];
u16 oam_ram[512];
u16 palette_ram_converted[512];
u16 io_registers[512];
u8 vram[1024 * 96];
u8 vram_clean[VRAM_DIRTY_MAP_BYTES];
u32 vram_dirty_marks;
u8 ewram[1024 * 256 * 2];
u8 iwram[1024 * 32 * 2];
u8 *memory_map_read[8 * 1024];
s32 affine_reference_x[2];
s32 affine_reference_y[2];

RFILE *filestream_open(const char *path, unsigned mode, unsigned hints)
{ (void)path; (void)mode; (void)hints; return NULL; }
int64_t filestream_seek(RFILE *stream, int64_t offset, int seek_position)
{ (void)stream; (void)offset; (void)seek_position; return -1; }
int64_t filestream_read(RFILE *stream, void *data, int64_t len)
{ (void)stream; (void)data; (void)len; return -1; }
int filestream_close(RFILE *stream)
{ (void)stream; return 0; }

void smc_prof_note_iwram(u32 offset) { (void)offset; }
void smc_prof_note_ewram(u32 offset) { (void)offset; }
cpu_alert_type flag_interrupt(irq_type irq_raised)
{ (void)irq_raised; return CPU_ALERT_NONE; }
cpu_alert_type check_interrupt(void) { return CPU_ALERT_NONE; }
cpu_alert_type write_siocnt(u16 value)
{ (void)value; return CPU_ALERT_NONE; }
cpu_alert_type write_rcnt(u16 value)
{ (void)value; return CPU_ALERT_NONE; }
void sound_timer_queue32(u32 channel, u32 value)
{ (void)channel; (void)value; }
void sound_reset_fifo(u32 channel) { (void)channel; }
void render_gbc_sound(void) {}

static unsigned g_rng = 0x2545F491u;
static u32 rnd(void)
{
  g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
  return g_rng;
}
static u32 rnd_colour(void)              /* converts to a non-zero value */
{
  u32 v;
  do v = rnd() & 0x7FFF; while (!v);
  return v;
}

static unsigned g_checks, g_fail;
#define CHECK(c, ...)                                                         \
  do { g_checks++;                                                            \
       if (!(c)) { if (g_fail++ < 12) { printf("FAIL: " __VA_ARGS__);         \
                                        putchar('\n'); } } } while (0)

/* Rows (32 B) of a 1 KiB array whose bytes differ between a and b. */
static u32 changed_rows(const void *a, const void *b)
{
  const u8 *x = (const u8 *)a, *y = (const u8 *)b;
  u32 m = 0, i;
  for (i = 0; i < 0x400; i++)
    if (x[i] != y[i])
      m |= 1u << (i >> 5);
  return m;
}

/* ------------------------------------------------------------------ rows */
static u32 rows_model(u32 a, u32 n)
{
  u32 m = 0, i;
  if (n >= 0x400)
    return ~0u;
  for (i = 0; i < n; i++)
    m |= 1u << (((a + i) & 0x3FF) >> 5);
  return m;
}

static void test_rows(void)
{
  u32 a, k;
  for (a = 0; a < 0x800; a++)
  {
    for (k = 0; k <= 70; k++)
      CHECK(me_wmap_rows(0x05000000 + a, k) == rows_model(a, k),
            "me_wmap_rows(%#x, %u)", a, k);
    for (k = 0; k < 24; k++)
    {
      u32 n = rnd() % 0x460;
      CHECK(me_wmap_rows(0x07000000 + a, n) == rows_model(a, n),
            "me_wmap_rows(%#x, %u)", a, n);
    }
  }
}

/* ------------------------------------------------------------- CPU stores */
static void test_cpu_stores(void)
{
  unsigned i;
  for (i = 0; i < 60000; i++)
  {
    u16 pal0[512], oam0[512];
    u32 region = (rnd() & 1) ? 5 : 7;
    u32 size = 1u << (rnd() % 3);                 /* 1, 2, 4 bytes */
    u32 addr = (region << 24) | (rnd() & 0xFFFFFF);
    u32 v = rnd_colour() | (rnd_colour() << 16);
    u32 map, want;
    addr &= ~(size - 1);
    memcpy(pal0, palette_ram_converted, sizeof(pal0));
    memcpy(oam0, oam_ram, sizeof(oam0));
    reg[PAL_WMAP] = reg[OAM_WMAP] = reg[PAL_UPDATED] = reg[OAM_UPDATED] = 0;
    if (size == 1)      write_memory8(addr, (u8)(v | 1));
    else if (size == 2) write_memory16(addr, (u16)v);
    else                write_memory32(addr, v);
    if (region == 5)
    {
      map = reg[PAL_WMAP];
      want = changed_rows(pal0, palette_ram_converted);
      CHECK(reg[PAL_UPDATED], "C palette store left PAL_UPDATED down");
      CHECK(!(want & ~map), "C palette %u-byte store %#x: rows %#x changed, "
            "map %#x", size, addr, want, map);
      CHECK(map == ~0u, "C palette %u-byte store %#x: map %#x, "
            "want all rows", size, addr, map);
    }
    else
    {
      map = reg[OAM_WMAP];
      want = changed_rows(oam0, oam_ram);
      CHECK(!(want & ~map), "C OAM %u-byte store %#x: rows %#x changed, "
            "map %#x", size, addr, want, map);
      if (size == 1)                            /* ignored by the GBA */
        CHECK(!map && !want && !reg[OAM_UPDATED], "C OAM byte store %#x "
              "marked %#x", addr, map);
      else
        CHECK(map == ~0u && reg[OAM_UPDATED], "C OAM %u-byte "
              "store %#x: map %#x, flag %u", size, addr, map, reg[OAM_UPDATED]);
    }
  }
}

/* -------------------------------------------------------------------- DMA */
static void prepare_dma(u32 src, u32 dst, u32 length,
  u32 src_direction, u32 dst_direction, u32 length_type)
{
  memset(&dma[0], 0, sizeof(dma[0]));
  dma[0].source_address = src;
  dma[0].dest_address = dst;
  dma[0].length = length;
  dma[0].source_direction = src_direction;
  dma[0].dest_direction = dst_direction;
  dma[0].length_type = length_type;
  dma[0].repeat_type = DMA_NO_REPEAT;
  dma[0].start_type = DMA_START_IMMEDIATELY;
  io_registers[REG_DMA0CNT_H] = 0x8000;
}

static void one_dma(u32 dst, u32 length, u32 dst_dir, u32 type)
{
  static const int stride[4] = { 1, -1, 0, 1 };
  u32 bytes = type == DMA_16BIT ? 2 : 4, i, a;
  u32 want_pal = 0, want_oam = 0, got_pal, got_oam;
  int cycles = 0;
  u32 d = 0x0FFFFFFF & dst & ~(bytes - 1);
  /* source: EWRAM, values that convert to non-zero, so every element it
   * writes changes its destination (cleared below) */
  for (i = 0; i < 0x2000; i += 2)
    *(u16 *)(ewram + i) = (u16)rnd_colour();
  memset(palette_ram, 0, sizeof(palette_ram));
  memset(palette_ram_converted, 0, sizeof(palette_ram_converted));
  memset(oam_ram, 0, sizeof(oam_ram));
  reg[PAL_WMAP] = reg[OAM_WMAP] = reg[PAL_UPDATED] = reg[OAM_UPDATED] = 0;
  /* the rows the elements land in, element by element */
  int crosses = 0;
  for (i = 0, a = d; i < length; i++, a += (u32)(stride[dst_dir] * (int)bytes))
  {
    if ((a >> 24) != (d >> 24) || ((a + bytes - 1) >> 24) != (d >> 24))
      crosses = 1;
    if ((a >> 24) == 5) want_pal |= rows_model(a, bytes);
    if ((a >> 24) == 7) want_oam |= rows_model(a, bytes);
  }
  prepare_dma(0x02000000, dst, length, DMA_INCREMENT, dst_dir, type);
  dma_transfer(0, &cycles);
  got_pal = reg[PAL_WMAP];
  got_oam = reg[OAM_WMAP];
  /* the capture only looks at a map when the flag is up */
  CHECK(!want_pal || reg[PAL_UPDATED], "DMA into the palette left "
        "PAL_UPDATED down");
  CHECK(!want_oam || reg[OAM_UPDATED], "DMA into OAM left OAM_UPDATED down");
  /* what really changed is exactly what the elements wrote... */
  {
    static const u16 zero[512];
    u32 ch_pal = changed_rows(zero, palette_ram_converted);
    u32 ch_oam = changed_rows(zero, oam_ram);
    CHECK(ch_pal == want_pal && ch_oam == want_oam, "model: DMA to %#x x%u "
          "dir %u %ub: changed pal %#x oam %#x, model %#x %#x", dst, length,
          dst_dir, bytes * 8, ch_pal, ch_oam, want_pal, want_oam);
    /* ...and the maps must cover it -- exactly, unless the run of elements
     * crosses a 16 MiB boundary (then every row is allowed) */
    CHECK(!(ch_pal & ~got_pal) && !(ch_oam & ~got_oam), "DMA to %#x x%u dir "
          "%u %ub: maps %#x %#x miss changed %#x %#x", dst, length, dst_dir,
          bytes * 8, got_pal, got_oam, ch_pal, ch_oam);
    if (!crosses)
    {
      CHECK(got_pal == ch_pal, "DMA to %#x x%u dir %u %ub: palette map %#x, "
            "changed %#x", dst, length, dst_dir, bytes * 8, got_pal, ch_pal);
      CHECK(got_oam == ch_oam, "DMA to %#x x%u dir %u %ub: OAM map %#x, "
            "changed %#x", dst, length, dst_dir, bytes * 8, got_oam, ch_oam);
    }
  }
}

static void test_dma(void)
{
  static const u32 bases[] = { 0x05000000, 0x07000000 };
  unsigned i;
  u32 dir, type;
  /* fixed shapes first: an HBlank gradient (1 colour, fixed / reloading),
   * a whole palette, a decrement from the region's first byte, a span
   * wrapping the 1 KiB mirror, and spans crossing a 16 MiB boundary */
  for (type = 0; type < 2; type++)
    for (dir = 0; dir < 4; dir++)
    {
      one_dma(0x05000004, 1, dir, type);
      one_dma(0x050001FE, 2, dir, type);
      one_dma(0x05400000, 512, dir, type);    /* stays in region 5 */
      one_dma(0x070003F0, 16, dir, type);
      one_dma(0x053FF3E0, 40, dir, type);
      one_dma(0x05FFFFF8, 8, dir, type);      /* into VRAM going up */
      one_dma(0x07000008, 8, dir, type);      /* into VRAM going down */
      one_dma(0x06FFFFF8, 8, dir, type);      /* from VRAM into OAM */
      one_dma(0x05000404, 0, dir, type);      /* length 0 */
    }
  for (i = 0; i < 40000; i++)
  {
    u32 base = bases[rnd() & 1];
    u32 off = (rnd() & 7) ? (rnd() & 0x7FF) : (rnd() & 0xFFFFFF);
    u32 len = (rnd() & 3) ? 1 + rnd() % 40 : 1 + rnd() % 700;
    u32 dir = rnd() & 3;
    if ((rnd() & 15) == 0)
      off = 0xFFFFFF - (rnd() & 0x3F);        /* near the region's end */
    /* a palette DMA decrementing below 0x05000000 would write I/O */
    if (base == 0x05000000 && dir == DMA_DECREMENT && off < len * 4)
      off += len * 4;
    one_dma(base + off, len, dir, rnd() & 1);
  }
}

int main(void)
{
  test_rows();
  test_cpu_stores();
  test_dma();
  if (g_fail)
  {
    printf("FAIL me_wmap: %u of %u checks\n", g_fail, g_checks);
    return 1;
  }
  printf("PASS me_wmap: %u checks (rows model, C stores, DMA)\n", g_checks);
  return 0;
}

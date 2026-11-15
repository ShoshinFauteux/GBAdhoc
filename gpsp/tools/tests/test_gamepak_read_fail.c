/* test_gamepak_read_fail.c -- a ROM file that goes away mid-run (code-health B-3).
 *
 * A console that cannot hold the whole cart (a PSP-1000 with a 32 MiB game)
 * reads ROM pages from the open file while the game runs (load_gamepak_page).
 * If the file is gone (the Memory Stick taken out, the file deleted) even a
 * reopen fails.  This drives the real gba_memory.c paging against an
 * in-memory "file" that can be made to vanish, and checks the contract:
 *
 *   - normal play is unchanged: a paged-in page is the file's bytes, the
 *     file's last partial page still ends in 0xFF, nothing is flagged;
 *   - a page the file cannot supply is mapped as ZEROS (no-ops in ARM and
 *     Thumb), never 0xFF (Thumb 0xFFFF is half a BL: a jump), and the sticky
 *     gamepak_read_failed goes up;
 *   - one reopen is tried per failure, not one per page while the flag is up;
 *   - gamepak_read_retry() unmaps exactly the failed pages, leaves good ones
 *     mapped, flushes the ROM translations and drops the flag;
 *   - with the file back, the next access reads the real bytes (reopening);
 *   - a ROM load clears the flag.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../gba_memory.c"

/* ---- the fake storage ------------------------------------------------- */
#define ROM_PAGES  64u                         /* 2 MiB: twice the cache */
#define ROM_BYTES  (ROM_PAGES * 32768u - 1000u) /* last page is partial */
static u8 rom_image[ROM_PAGES * 32768u];
static int file_gone;        /* the card is out: opens fail */
static int handle_dead;      /* the open handle died (sleep, removal) */
static int64_t file_pos;
static unsigned opens, flushes;
static int token;

RFILE *filestream_open(const char *path, unsigned mode, unsigned hints)
{
  (void)path; (void)mode; (void)hints;
  opens++;
  if (file_gone)
    return NULL;
  handle_dead = 0;
  file_pos = 0;
  return (RFILE *)&token;
}
int64_t filestream_get_size(RFILE *stream)
{ (void)stream; return ROM_BYTES; }
int64_t filestream_seek(RFILE *stream, int64_t offset, int seek_position)
{
  (void)stream; (void)seek_position;
  if (handle_dead)
    return -1;
  file_pos = offset;
  return 0;
}
int64_t filestream_read(RFILE *stream, void *data, int64_t len)
{
  int64_t n;
  (void)stream;
  if (handle_dead)
    return -1;
  n = (int64_t)ROM_BYTES - file_pos;
  if (n > len) n = len;
  if (n < 0) n = 0;
  memcpy(data, rom_image + file_pos, (size_t)n);
  file_pos += n;
  return n;
}
int filestream_close(RFILE *stream) { (void)stream; return 0; }

/* ---- link stubs for the parts of gba_memory.c this test never runs ---- */
u8 *memory_map_read[8 * 1024];
void flush_translation_cache_rom(void) { flushes++; }

static u8 *mapped(u32 page)
{
  return memory_map_read[(0x8000000 >> 15) + page];
}

static void check_page(const u8 *p, u32 page)
{
  u32 i, end = 32768u;
  if ((page + 1) * 32768u > ROM_BYTES)
    end = ROM_BYTES - page * 32768u;
  for (i = 0; i < end; i++)
    assert(p[i] == (u8)(page + 1));
  for (; i < 32768u; i++)
    assert(p[i] == 0xFF);                   /* past the file's end */
}

static void check_zero(const u8 *p)
{
  u32 i;
  for (i = 0; i < 32768u; i++)
    assert(p[i] == 0);
}

int main(void)
{
  u32 i;
  u8 *p, *q, *good;

  for (i = 0; i < ROM_PAGES; i++)
    memset(rom_image + i * 32768u, (int)(i + 1), 32768u);

  gamepak_buffer_cap = 1;                   /* a 1 MiB cache: 32 pages */
  init_gamepak_buffer();
  assert(gamepak_buffer_pages == 32);
  assert(load_gamepak_raw("rom.gba") == 0);
  assert(gamepak_must_swap());
  check_page(mapped(0), 0);
  check_page(mapped(31), 31);
  assert(!mapped(40));

  /* 1. Normal paging: the file's bytes, the partial page's 0xFF tail. */
  good = load_gamepak_page(40);
  assert(good == mapped(40));
  check_page(good, 40);
  check_page(load_gamepak_page(ROM_PAGES - 1), ROM_PAGES - 1);
  assert(!gamepak_read_failed && gamepak_reopens == 0 &&
         gamepak_short_reads == 0);

  /* 2. A dead handle with the file still there: the reopen recovers it,
   * nothing is flagged (the 2026 Heart & Soul sleep fix, unchanged). */
  handle_dead = 1;
  check_page(load_gamepak_page(41), 41);
  assert(gamepak_reopens == 1 && !gamepak_read_failed);

  /* 3. The card is out: zeros, flagged, one reopen. */
  handle_dead = 1;
  file_gone = 1;
  opens = 0;
  p = load_gamepak_page(50);
  assert(p == mapped(50));
  check_zero(p);
  assert(gamepak_read_failed == 1);
  assert(gamepak_reopens == 2 && gamepak_short_reads == 1 && opens == 1);

  /* ...and a frame that slides on into the next page pays no reopen. */
  q = load_gamepak_page(51);
  check_zero(q);
  assert(opens == 1 && gamepak_reopens == 2 && gamepak_short_reads == 2);
  assert(mapped(40) == good);               /* good pages stay mapped */

  /* 4. Retry with the card still out: the failed pages are unmapped, the
   * ROM translations flushed, the flag down; the next access fails again
   * (one reopen) and raises it again. */
  flushes = 0;
  gamepak_read_retry();
  assert(!gamepak_read_failed && flushes == 1);
  assert(!mapped(50) && !mapped(51));
  assert(mapped(40) == good);
  check_page(good, 40);
  check_zero(load_gamepak_page(50));
  assert(gamepak_read_failed && opens == 2);

  /* 5. The card is back: the retry and the next fault read real bytes. */
  file_gone = 0;
  gamepak_read_retry();
  assert(!gamepak_read_failed && !mapped(50));
  check_page(load_gamepak_page(50), 50);
  check_page(load_gamepak_page(51), 51);
  assert(!gamepak_read_failed);
  assert(mapped(40) == good);

  /* 6. Every queue entry that held a failed page was released: run the
   * whole cache through twice and every page still reads true. */
  for (i = 0; i < 2 * ROM_PAGES; i++)
    check_page(load_gamepak_page(i % ROM_PAGES), i % ROM_PAGES);
  assert(!gamepak_read_failed);

  /* 7. A failure is forgotten by the next ROM load. */
  file_gone = 1;
  handle_dead = 1;
  for (i = 0; i < 1024 && !gamepak_read_failed; i++)
    (void)load_gamepak_page(32 + (i % 32));
  assert(gamepak_read_failed);
  file_gone = 0;
  assert(load_gamepak_raw("rom.gba") == 0);
  assert(!gamepak_read_failed);
  check_page(load_gamepak_page(45), 45);

  memory_term();
  puts("PASS gamepak read failure: zeros not 0xFF, sticky flag, one reopen, "
       "retry unmaps only the failed pages and flushes, file back reads true");
  return 0;
}

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
 *   - a ROM load clears the flag;
 *   - the frontend's cycle (code-health C-0, step 8): gamepak_read_poll() at
 *     every frame boundary pauses ONCE per failure, with one rom_read_failed
 *     event, and retries at the next boundary; a failure after the retry
 *     pauses again (F1), and nothing of one game's failure silences the next
 *     game's (F2, F3).
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

/* ---- the frontend's poll (step 8) --------------------------------------
 * rom_poll() is one frame boundary as the PSP frontend runs it: fe_host.c
 * fe_host_rom_read_poll, which logs EVT rom_read_failed on PAUSE and EVT
 * rom_read_retry on RETRIED, counted here.  A file given with -include that
 * defines TEST_ROM_POLL replaces it (the pre-C-0 frontend, transcribed, for
 * the fail-before run; not kept in the repo, docs/design/TIER-C.md C-0). */
#ifndef TEST_ROM_POLL
static unsigned ev_failed, ev_retry;
static int rom_poll(void)
{
  int r = gamepak_read_poll();
  if (r == GAMEPAK_READ_PAUSE)
    ev_failed++;
  else if (r == GAMEPAK_READ_RETRIED)
    ev_retry++;
  return r;
}
#endif

/* Step 8's checks report every failure and go on, so one run shows each
 * sub-step's verdict; each sub-step starts from a drained state. */
static int fails8;
#define EXPECT(cond, what) do { if (!(cond)) { fails8++; \
    printf("FAIL %s: %s (line %d)\n", what, #cond, __LINE__); } } while (0)

static void drain(void)
{
  int i;
  file_gone = 0;
  for (i = 0; i < 8 && rom_poll() != GAMEPAK_READ_OK; i++)
    ;
}

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

  /* 8. The frontend's cycle (code-health C-0): one poll per frame boundary;
   * the menu the PAUSE opens runs no frame, so the next poll is the Resume. */
  drain();
  ev_failed = ev_retry = 0;
  EXPECT(rom_poll() == GAMEPAK_READ_OK, "8.0 nothing failed");

  /* 8.1 Card out: the page fails, the boundary pauses with one event, the
   * Resume retries (failed page unmapped, ROM translations flushed). */
  handle_dead = 1;
  file_gone = 1;
  flushes = 0;
  check_zero(load_gamepak_page(50));
  EXPECT(rom_poll() == GAMEPAK_READ_PAUSE, "8.1 the failure pauses");
  EXPECT(ev_failed == 1, "8.1 one rom_read_failed event");
  EXPECT(rom_poll() == GAMEPAK_READ_RETRIED, "8.1 Resume retries");
  EXPECT(ev_retry == 1 && flushes == 1 && !mapped(50), "8.1 the retry");

  /* 8.2 (F1) Card still out: the retried page fails again.  It must pause
   * again; the pre-C-0 frontend retried here, every frame, never pausing. */
  check_zero(load_gamepak_page(50));
  EXPECT(rom_poll() == GAMEPAK_READ_PAUSE, "8.2 F1 a second failure pauses");
  EXPECT(ev_failed == 2, "8.2 F1 a second rom_read_failed event");
  EXPECT(rom_poll() == GAMEPAK_READ_RETRIED, "8.2 Resume retries");

  /* 8.3 Card back: the next access reads true and nothing is pending. */
  drain();
  file_gone = 0;
  check_page(load_gamepak_page(50), 50);
  EXPECT(rom_poll() == GAMEPAK_READ_OK, "8.3 the card back reads true");

  /* 8.4 (F2) A failure, the pause, then Quit to game list and the next game
   * (a ROM load); its first boundary is quiet.  The next game's failure must
   * log its own rom_read_failed: the pre-C-0 `seen` latch outlived the game
   * and swallowed it (the pause still came; Lab fixtures key on the event). */
  drain();
  ev_failed = ev_retry = 0;
  handle_dead = 1;
  file_gone = 1;
  check_zero(load_gamepak_page(50));
  EXPECT(rom_poll() == GAMEPAK_READ_PAUSE, "8.4 the failure pauses");
  file_gone = 0;
  assert(load_gamepak_raw("rom.gba") == 0);     /* Quit; the next game */
  EXPECT(rom_poll() == GAMEPAK_READ_OK, "8.4 the next game starts clean");
  handle_dead = 1;
  file_gone = 1;
  check_zero(load_gamepak_page(50));
  EXPECT(rom_poll() == GAMEPAK_READ_PAUSE, "8.4 the next game's failure pauses");
  EXPECT(ev_failed == 2, "8.4 F2 the next game's failure logs rom_read_failed");

  /* 8.5 (F3) The same, but the next game's page fails before its first
   * boundary (inside the session load): it must pause, with its event; the
   * pre-C-0 latch was still set and retried silently instead. */
  drain();
  ev_failed = ev_retry = 0;
  handle_dead = 1;
  file_gone = 1;
  check_zero(load_gamepak_page(50));
  EXPECT(rom_poll() == GAMEPAK_READ_PAUSE, "8.5 the failure pauses");
  file_gone = 0;
  assert(load_gamepak_raw("rom.gba") == 0);     /* Quit; the next game */
  handle_dead = 1;
  file_gone = 1;
  check_zero(load_gamepak_page(51));            /* before its first boundary */
  EXPECT(rom_poll() == GAMEPAK_READ_PAUSE, "8.5 F3 a failure in the load pauses");
  EXPECT(ev_failed == 2, "8.5 F3 ...with its rom_read_failed event");

  /* 8.6 A failure while paused (a guest frame the open menu ran: rewind)
   * is a new failure: one more pause, then the retry.  Intended. */
  drain();
  ev_failed = ev_retry = 0;
  handle_dead = 1;
  file_gone = 1;
  check_zero(load_gamepak_page(50));
  EXPECT(rom_poll() == GAMEPAK_READ_PAUSE, "8.6 the failure pauses");
  check_zero(load_gamepak_page(52));            /* a frame inside the menu */
  EXPECT(rom_poll() == GAMEPAK_READ_PAUSE, "8.6 the menu's failure pauses once more");
  EXPECT(rom_poll() == GAMEPAK_READ_RETRIED, "8.6 then Resume retries");
  EXPECT(ev_failed == 2 && ev_retry == 1, "8.6 two failures, one retry logged");
  file_gone = 0;
  check_page(load_gamepak_page(52), 52);
  EXPECT(rom_poll() == GAMEPAK_READ_OK, "8.6 the card back reads true");

  memory_term();
  if (fails8)
  {
    printf("FAIL gamepak read failure: %d check(s) of the frontend cycle (step 8)\n",
           fails8);
    return 1;
  }
  puts("PASS gamepak read failure: zeros not 0xFF, sticky flag, one reopen, "
       "retry unmaps only the failed pages and flushes, file back reads true, "
       "the frontend's poll pauses once per failure with its event");
  return 0;
}

/* test_gamepak_residency_budget.c -- the ROM page cache's startup budget.
 *
 * init_gamepak_buffer() decides whether a cart is held whole (docs/
 * ROM-RESIDENCY.md, "One startup memory budget").  This drives the real
 * function (the PSP branch, selected by GPSP_TEST_GAMEPAK_BUDGET) against a
 * malloc with a hard byte budget, and checks the rules the design rests on:
 *
 *   - a resident cart leaves at least the 1 MiB post-load floor free;
 *   - a short heap hands every extra block back: the result is the paged
 *     layout, with exactly the heap left that the paged path leaves;
 *   - a PSP-1000-sized heap makes the same successful mallocs with or
 *     without the wish;
 *   - blocks lent from the spare pool are used first and never freed.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HDR 16u                 /* a generous per-chunk malloc overhead */
#define MIB (1024u * 1024u)
#define MAX_LIVE 256

static size_t budget_left;      /* bytes this "heap" can still hand out */
static void  *live_ptr[MAX_LIVE];
static size_t live_len[MAX_LIVE];
static unsigned mallocs_ok, mallocs_failed;

static void *budget_malloc(size_t n)
{
  unsigned i;
  if (n + HDR > budget_left)
  {
    mallocs_failed++;
    return NULL;
  }
  for (i = 0; i < MAX_LIVE; i++)
    if (!live_ptr[i])
    {
      live_ptr[i] = malloc(n ? n : 1);
      assert(live_ptr[i]);
      live_len[i] = n + HDR;
      budget_left -= n + HDR;
      mallocs_ok++;
      return live_ptr[i];
    }
  assert(!"live table full");
  return NULL;
}

static void budget_free(void *p)
{
  unsigned i;
  if (!p)
    return;
  for (i = 0; i < MAX_LIVE; i++)
    if (live_ptr[i] == p)
    {
      free(p);
      budget_left += live_len[i];
      live_ptr[i] = NULL;
      return;
    }
  /* Freeing memory the heap never handed out (a spare-pool block) is the
   * bug this test exists to catch. */
  assert(!"free of a pointer the heap does not own");
}

#define GPSP_TEST_GAMEPAK_BUDGET 1
#define malloc budget_malloc
#define free budget_free
#include "../gba_memory.c"
#undef malloc
#undef free

/* ---- link stubs for the parts of gba_memory.c this test never runs ---- */
u8 *memory_map_read[8 * 1024];
RFILE *filestream_open(const char *path, unsigned mode, unsigned hints)
{ (void)path; (void)mode; (void)hints; return NULL; }
int64_t filestream_get_size(RFILE *stream) { (void)stream; return 0; }
int64_t filestream_seek(RFILE *stream, int64_t offset, int seek_position)
{ (void)stream; (void)offset; (void)seek_position; return 0; }
int64_t filestream_read(RFILE *stream, void *data, int64_t len)
{ (void)stream; (void)data; (void)len; return -1; }
int filestream_close(RFILE *stream) { (void)stream; return 0; }

static u8 spare[2 * MIB];

static void reset(size_t heap, u32 cap, u32 want, int with_spare)
{
  memory_term();
  budget_left = heap;
  mallocs_ok = mallocs_failed = 0;
  gamepak_buffer_cap = cap;
  gamepak_resident_wanted = want;
  gamepak_spare_pool = with_spare ? spare : NULL;
  gamepak_spare_pool_bytes = with_spare ? (u32)sizeof(spare) : 0;
}

static void check_released_all(size_t heap)
{
  memory_term();
  assert(budget_left == heap);   /* every heap block came back, no more */
}

int main(void)
{
  size_t heap, left_paged, left_wish;
  unsigned ok_paged, ok_wish;
  u32 i;

  /* 1. PSP-1000 shape: the heap runs out at 13 blocks.  The wish must not
   * change a single successful malloc, nor the heap left afterwards. */
  heap = 1 * MIB + HDR + 13 * (MIB + HDR) + 700 * 1024;
  reset(heap, 15, 0, 0);
  init_gamepak_buffer();
  assert(gamepak_buffer_count == 13);
  assert(strcmp(gamepak_cap_reason, "default") == 0);
  left_paged = budget_left;
  ok_paged = mallocs_ok;
  assert(left_paged >= MIB);                 /* the post-load floor */
  check_released_all(heap);

  reset(heap, 15, 32, 0);
  init_gamepak_buffer();
  assert(gamepak_buffer_count == 13);
  assert(strcmp(gamepak_cap_reason, "heap_short") == 0);
  assert(gamepak_resident_got == 13);
  left_wish = budget_left;
  ok_wish = mallocs_ok;
  assert(left_wish == left_paged && ok_wish == ok_paged);
  check_released_all(heap);

  /* 2. A real PSP Go beside the LARGE JIT tier: ~31.6 MiB of heap at the
   * decision, 2 MiB lent from the idle static JIT array.  Resident, with
   * the floor intact.  (Without the pool the same heap must page.) */
  heap = 33145560;   /* measured-derived, see docs/ROM-RESIDENCY.md */
  reset(heap, 15, 32, 1);
  init_gamepak_buffer();
  assert(strcmp(gamepak_cap_reason, "resident") == 0);
  assert(gamepak_buffer_count == 32 && gamepak_static_blocks == 2);
  assert(gamepak_buffers[0] == spare && gamepak_buffers[1] == spare + MIB);
  assert(budget_left >= MIB);
  check_released_all(heap);

  reset(heap, 15, 32, 0);
  init_gamepak_buffer();
  assert(strcmp(gamepak_cap_reason, "heap_short") == 0);
  assert(gamepak_resident_got < 32);
  assert(gamepak_buffer_count == 15 && gamepak_static_blocks == 0);
  check_released_all(heap);

  /* 3. Short by one block: every extra block goes back and the result is
   * exactly the paged path's (count and heap left). */
  heap = 1 * MIB + HDR + 29 * (MIB + HDR) + 512 * 1024;
  reset(heap, 15, 0, 1);
  init_gamepak_buffer();
  assert(gamepak_buffer_count == 15 && gamepak_static_blocks == 2);
  left_paged = budget_left;
  check_released_all(heap);
  reset(heap, 15, 32, 1);
  init_gamepak_buffer();
  assert(strcmp(gamepak_cap_reason, "heap_short") == 0);
  assert(gamepak_resident_got == 31);
  assert(gamepak_buffer_count == 15 && gamepak_static_blocks == 2);
  assert(budget_left == left_paged);
  for (i = 0; i < gamepak_buffer_count; i++)
    assert(gamepak_buffers[i] != NULL);
  check_released_all(heap);

  /* 4. The wish is the cart's size: a 16 MiB cart asks for 16, fits where
   * 32 would not, and a cart the default cap holds makes no attempt. */
  heap = 1 * MIB + HDR + 20 * (MIB + HDR);
  reset(heap, 15, 16, 0);
  init_gamepak_buffer();
  assert(strcmp(gamepak_cap_reason, "resident") == 0);
  assert(gamepak_buffer_count == 16 && budget_left >= MIB);
  check_released_all(heap);
  reset(heap, 15, 8, 0);
  init_gamepak_buffer();
  assert(strcmp(gamepak_cap_reason, "default") == 0);
  assert(gamepak_resident_got == 0 && gamepak_buffer_count == 15);
  check_released_all(heap);

  /* 5. The floor is exact: one byte less than 32 blocks + the reserve
   * refuses residency; exactly enough grants it. */
  heap = 1 * MIB + HDR + 32 * (MIB + HDR);
  reset(heap, 15, 32, 0);
  init_gamepak_buffer();
  assert(strcmp(gamepak_cap_reason, "resident") == 0);
  assert(budget_left == MIB + HDR);
  check_released_all(heap);
  reset(heap - 1, 15, 32, 0);
  init_gamepak_buffer();
  assert(strcmp(gamepak_cap_reason, "heap_short") == 0);
  assert(gamepak_buffer_count == 15);
  check_released_all(heap - 1);

  puts("PASS gamepak residency budget: floor kept, short heap pages, "
       "1000 unchanged, spare pool used first and never freed");
  return 0;
}

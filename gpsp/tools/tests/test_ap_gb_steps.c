/* test_ap_gb_steps.c -- the autopilot commands a GB link session's scripts
 * rely on, against a model of a walking Game Boy player whose buttons
 * arrive D frames late (the session's input delay).
 *
 *   stepram  reaches the exact tile for every D the link offers (0..24),
 *   holdram  -- the reason stepram exists -- overshoots once D > 0,
 *   ifram    runs or skips the next N steps on a RAM value.
 *
 * The model: a player standing still starts a one-tile step when the
 * direction is held at a decision frame; a step takes 16 frames; the tile
 * coordinate changes when the step completes (as wXCoord does).  Built with
 * the real fe_autopilot.c.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void fe_log(const char *fmt, ...) { (void)fmt; }
void fe_evt(const char *fmt, ...) { (void)fmt; }
unsigned long long fe_evt_now_us(void) { return 0; }
uint32_t fe_host_sram_crc_now(void) { return 0; }

static uint8_t mem[0x10000];
static uint32_t inject;
void fe_host_input_inject(uint32_t m) { inject = m; }
int fe_host_mem_read(uint32_t a, void *o, unsigned n)
{
  if (a + n > sizeof(mem)) return -1;
  memcpy(o, mem + a, n);
  return 0;
}

#include "fe_autopilot.c"

#define X 0xD362
#define RIGHT_BIT (1u << 7)

static uint32_t pipe[64];

/* Run a script against the walker with input delay d; return final x. */
static int walk(const char *script, unsigned d, unsigned frames)
{
  char path[] = "/tmp/t_ap_gb_XXXXXX";
  int fd = mkstemp(path);
  FILE *f = fdopen(fd, "w");
  int step_left = 0;
  unsigned t;
  fputs(script, f);
  fclose(f);
  memset(mem, 0, sizeof(mem));
  memset(pipe, 0, sizeof(pipe));
  mem[X] = 10;
  inject = 0;
  if (fe_autopilot_load(path) != 0) { remove(path); return -1; }
  remove(path);
  for (t = 0; t < frames; t++)
  {
    uint32_t pressed;
    fe_autopilot_frame();
    pipe[(t + d) % 64] = inject;
    pressed = pipe[t % 64];
    pipe[t % 64] = 0;
    if (step_left)
    {
      if (--step_left == 0)
        mem[X]++;
    }
    else if (pressed & RIGHT_BIT)
      step_left = 16;
  }
  return mem[X];
}

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, \
  __LINE__, #c); return 1; } } while (0)

int main(void)
{
  unsigned d;
  unsigned over = 0;
  for (d = 0; d <= 24; d++)
  {
    int x = walk("stepram RIGHT 1 0xD362 0xFF 15 2000\nwait 200\n", d, 800);
    CHECK(x == 15);
    if (walk("holdram RIGHT 1 0xD362 0xFF 15 2000\nwait 200\n", d, 800) > 15)
      over++;
  }
  CHECK(over >= 20);     /* holdram overshoots for (nearly) every delay > 0 */
  /* ifram: taken when equal, skipped otherwise. */
  CHECK(walk("ifram 1 0xD362 0xFF 10 1\nstepram RIGHT 1 0xD362 0xFF 12 900\n"
             "wait 100\n", 3, 400) == 12);
  CHECK(walk("ifram 1 0xD362 0xFF 11 1\nstepram RIGHT 1 0xD362 0xFF 12 900\n"
             "wait 100\n", 3, 400) == 10);
  CHECK(walk("ifram 1 0xD362 0xFF 11 2\nwait 10\nwait 10\n"
             "stepram RIGHT 1 0xD362 0xFF 11 900\n", 0, 400) == 11);
  printf("autopilot GB steps: stepram exact for input delay 0..24, holdram "
         "overshoots in %u of 25 delays, ifram takes/skips\n", over);
  return 0;
}

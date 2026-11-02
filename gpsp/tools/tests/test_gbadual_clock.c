/* Actual session lifecycle, injected deserialize failures, no PSP/ROM. */
#include <assert.h>
#include <stdint.h>
uint32_t gbalink_fast, gbab_gbalink_fast, backup_writes, gbab_backup_writes;
extern volatile uint32_t gbalink_abort, gbab_gbalink_abort;
extern char gbab_bdata_start[] __attribute__((weak));
extern char gbab_bdata_end[] __attribute__((weak));
extern char gbab_bdata_pristine[] __attribute__((weak));
extern char gbab_bregion_start[] __attribute__((weak));
extern char gbab_bregion_lend[] __attribute__((weak));
extern uint8_t *gamepak_spare_pool2;
extern uint32_t gamepak_spare_pool2_bytes;
extern uint32_t gbalink_headless_audio, gbab_gbalink_headless_audio;
extern uint32_t sched_fast __attribute__((weak)), gbab_sched_fast __attribute__((weak));
extern uint32_t timer_lazy __attribute__((weak)), gbab_timer_lazy __attribute__((weak));
extern uint32_t irq_io_fast __attribute__((weak)), gbab_irq_io_fast __attribute__((weak));
extern uint32_t msr_fast __attribute__((weak)), gbab_msr_fast __attribute__((weak));
#define GBA_LINK
#include "../../frontend-common/fe_gbadual.c"

time_t (*gpsp_wallclock)(void);
time_t (*gbab_gpsp_wallclock)(void);
int gpsp_rtc_frames_only;
void (*gamepak_evict_hook)(uint32_t);
static time_t real_clock(void) { return 1791504000; }
static time_t b_clock(void) { return 1791504001; }
static unsigned calls_a, calls_b, fail_a, fail_b, end_calls;
static uint8_t ram_a[0x20000], ram_b[0x20000];

size_t retro_serialize_size(void) { return 1; }
size_t gbab_retro_serialize_size(void) { return 1; }
bool retro_unserialize(const void *p, size_t n)
{ (void)p; (void)n; calls_a++; return !fail_a; }
bool gbab_retro_unserialize(const void *p, size_t n)
{ (void)p; (void)n; calls_b++; return !fail_b; }
void *retro_get_memory_data(unsigned k) { (void)k; return ram_a; }
void *gbab_retro_get_memory_data(unsigned k) { (void)k; return ram_b; }
static int boundary(void) { return 1; }
void gbalink_pair_init(gbalink_pair *p, const gbalink_inst_ops *a,
 const gbalink_inst_ops *b, const gbalink_pair_config *c)
{
 (void)a; (void)b; p->cfg = *c;
 p->m[0].need_begin = boundary; p->m[1].need_begin = boundary;
}
void gbalink_pair_end(gbalink_pair *p) { (void)p; end_calls++; }
int gbalink_pair_step(gbalink_pair *p) { (void)p; return GBALINK_PAIR_FRAME; }
void gbalink_script_free(gbalink_script *p) { (void)p; }

static void fresh(int prior_frames)
{
  memset(&D, 0, sizeof(D)); D.loaded = 1;
  gpsp_wallclock = real_clock; gbab_gpsp_wallclock = b_clock;
  gpsp_rtc_frames_only = prior_frames;
  calls_a = calls_b = fail_a = fail_b = end_calls = 0;
  memset(ram_a, 0x5a, sizeof(ram_a)); memset(ram_b, 0xa5, sizeof(ram_b));
}
static void restored(int prior_frames)
{
  assert(!sess_clock_on && gpsp_wallclock == real_clock);
  assert(gbab_gpsp_wallclock == b_clock);
  assert(gpsp_rtc_frames_only == prior_frames);
}
static int start(int local)
{
  const uint8_t b = 0, av = 0x12, bv = 0x34;
  const uint8_t *states[2] = {&b, &b}, *saves[2] = {&av, &bv};
  size_t sizes[2] = {1, 1};
  return m_start(NULL, local, 1, 1791504000, states, sizes, saves, sizes, NULL, NULL);
}
int main(void)
{
  int local, prior;
  for (prior = 0; prior <= 1; prior++)
    for (local = 0; local < 2; local++)
    {
      fresh(prior); fail_b = 1;
      assert(start(local) < 0 && calls_a == 0 && calls_b == 1);
      assert(ram_a[0] == 0x5a && ram_b[0] == 0xa5);
      restored(prior); fe_gbadual_close(); restored(prior);
      fresh(prior); fail_a = 1;
      assert(start(local) < 0 && calls_a == 1 && calls_b == 1);
      assert(ram_a[0] == 0x5a && ram_b[0] == 0xa5);
      restored(prior);
      fail_a = 0; assert(start(local) == 0); /* retry after failure */
      assert(gpsp_wallclock != real_clock && gpsp_rtc_frames_only == 1);
      assert(ram_a[0] == (local ? 0x34 : 0x12));
      assert(ram_b[0] == (local ? 0x12 : 0x34));
      m_stop(NULL); restored(prior); assert(end_calls == 1);
      m_stop(NULL); fe_gbadual_close(); restored(prior); /* idempotent */
      fresh(prior); assert(start(local) == 0);
      fe_gbadual_close(); restored(prior); assert(end_calls == 1);
      fresh(prior); assert(start(local) == 0);
      gbadual_stop("injected fault"); restored(prior); assert(D.failed);
    }
  {
    const uint8_t b = 0;
    const uint8_t *states[2] = {&b, &b}, *saves[2] = {&b, &b};
    size_t sizes[2] = {1, 2}, sn[2] = {1, 1};
    fresh(0);
    assert(m_start(NULL, 0, 1, 0, states, sizes, saves, sn, NULL, NULL) < 0);
    assert(calls_a == 0 && calls_b == 0); restored(0);
    sizes[1] = 1; sn[1] = 0x20001;
    assert(m_start(NULL, 0, 1, 0, states, sizes, saves, sn, NULL, NULL) < 0);
    assert(calls_a == 0 && calls_b == 0); restored(0);
  }
  puts("GBA dual clock: both slots, failed A/B, retry, stop, close, fault, prior hooks/mode, SRAM transaction, size rejection PASS");
  return 0;
}

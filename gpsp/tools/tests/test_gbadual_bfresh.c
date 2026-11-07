/* b_fresh with real section markers: instance B's .data, region, .sdata and
 * .sbss go back to fresh-boot bytes at EVERY session start, the second one
 * included (critique-5 SF1: the markers are NULL in every other host test, so
 * the copy and the zero never ran anywhere).  No PSP, no ROM. */
#include <assert.h>
#include <stdint.h>
uint32_t gbalink_fast, gbab_gbalink_fast, backup_writes, gbab_backup_writes;
#include <string.h>
#include <stdio.h>
extern volatile uint32_t gbalink_abort, gbab_gbalink_abort;
/* Instance B's sections, laid out here the way gbalink_core_b.ld lays them
 * out on the PSP: each marker pair bounds one buffer. */
char bfx_data[32], bfx_pristine[32], bfx_region[256], bfx_sdata[12], bfx_sbss[64];
__asm__(".globl gbab_bdata_start\n.set gbab_bdata_start, bfx_data\n"
        ".globl gbab_bdata_end\n.set gbab_bdata_end, bfx_data+32\n"
        ".globl gbab_bdata_pristine\n.set gbab_bdata_pristine, bfx_pristine\n"
        ".globl gbab_bregion_start\n.set gbab_bregion_start, bfx_region\n"
        ".globl gbab_bregion_lend\n.set gbab_bregion_lend, bfx_region+256\n"
        ".globl gbab_bsdata_start\n.set gbab_bsdata_start, bfx_sdata\n"
        ".globl gbab_bsdata_end\n.set gbab_bsdata_end, bfx_sdata+12\n"
        ".globl gbab_bsbss_start\n.set gbab_bsbss_start, bfx_sbss\n"
        ".globl gbab_bsbss_end\n.set gbab_bsbss_end, bfx_sbss+64\n");
#include "../../frontend-common/fe_gbadual_b.h"
uint8_t *gamepak_spare_pool2;
uint32_t gamepak_spare_pool2_bytes;
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

static unsigned char want_data[32], want_sdata[12];
static int all(const char *p, size_t n, int v)
{
  size_t i;
  for (i = 0; i < n; i++) if ((unsigned char)p[i] != v) return 0;
  return 1;
}
/* What a session leaves behind: every byte of B's state changed. */
static void session(int v)
{
  memset(bfx_data, v, sizeof(bfx_data)); memset(bfx_region, v, sizeof(bfx_region));
  memset(bfx_sdata, v, sizeof(bfx_sdata)); memset(bfx_sbss, v, sizeof(bfx_sbss));
}
static void fresh_ok(void)
{
  assert(!memcmp(bfx_data, want_data, sizeof(want_data)));
  assert(all(bfx_region, sizeof(bfx_region), 0));
  assert(!memcmp(bfx_sdata, want_sdata, sizeof(want_sdata)));
  assert(all(bfx_sbss, sizeof(bfx_sbss), 0));
}
int main(void)
{
  int i;
  (void)ram_a; (void)ram_b; (void)real_clock; (void)b_clock; (void)calls_a;
  (void)calls_b; (void)fail_a; (void)fail_b; (void)end_calls;
  for (i = 0; i < 32; i++) bfx_data[i] = (char)(0x40 + i);
  for (i = 0; i < 12; i++) bfx_sdata[i] = (char)(0x80 + i);   /* rfu_sched_ni_at .. */
  memcpy(want_data, bfx_data, 32); memcpy(want_sdata, bfx_sdata, 12);
  fe_gbadual_boot();                    /* the boot snapshot */
  assert(g_bpristine && g_bsdata_ok);
  fe_gbadual_boot();                    /* idempotent: a second boot keeps the first copy */
  for (i = 1; i <= 3; i++)              /* sessions 1, 2 and 3 */
  {
    session(0xE0 + i);
    b_fresh();
    fresh_ok();
  }
  /* The region is what solo play lends to A's ROM cache; the lend covers
   * exactly the marker span. */
  fe_gbadual_lend(1);
  assert(fe_gbadual_lent() && fe_gbadual_lendable() == 256);
  assert(gamepak_spare_pool2 == (uint8_t *)gbab_bregion_start);
  fe_gbadual_lend(0);
  assert(!fe_gbadual_lent());
  puts("GBA dual B fresh: .data, region, .sdata, .sbss restored at sessions 1-3; lend spans the region PASS");
  return 0;
}

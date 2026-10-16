/* Exercise the production frontend's response to untrusted replay state. */
#include <assert.h>
#include "../../frontend-common/fe_host.c"

static int injected_result, reset_count;
static uint8_t battery[FE_SRAM_SIZE];
static fe_rewind_stats stats;
uint32_t gba_rewind_backup_ro, gba_rewind_backup_ro_hits;
uint32_t gba_rewind_rlog_miss, gba_rewind_rlog_pos, gba_rewind_rlog_end;
uint32_t gba_rewind_bk_acc;
volatile uint32_t backup_write_gen;
void fe_evt(const char *fmt, ...) { (void)fmt; }
void fe_log(const char *fmt, ...) { (void)fmt; }
void retro_reset(void) { reset_count++; }
void *retro_get_memory_data(unsigned id) { (void)id; return battery; }
size_t retro_get_memory_size(unsigned id) { (void)id; return sizeof(battery); }
int fe_np_session_active(void) { return 0; }
void gpsp_rewind_rprof(uint32_t out[9]) { memset(out, 0, 9*sizeof(*out)); }
void gpsp_rewind_clock_sync(void) {}
void fe_rewind_reset(uint32_t f) { (void)f; injected_result = FE_RW_PLAY; }
uint32_t fe_rewind_frame_mach(void) { return 20; }
uint32_t fe_rewind_frame_now(void) { return 25; }
uint32_t fe_rewind_frame_top(void) { return 30; }
uint32_t fe_rewind_frame_oldest(void) { return 0; }
int fe_rewind_tick_step(int h, uint32_t n, fe_rewind_shown *s, uint32_t *steps)
{ (void)h; (void)n; (void)s; *steps=0; return injected_result; }
const fe_rewind_stats *fe_rewind_get_stats(void) { return &stats; }
int fe_rewind_at_limit(void) { return 0; }
uint32_t gba_rewind_backup_busy(void) { return 0; }
uint32_t fe_crc32(uint32_t crc, const void *p, size_t n)
{ const uint8_t *b=p; while(n--) crc=crc*33+*b++; return crc; }

static void setup(void)
{
   rw_on=1; rw_gesture=1; rw_present_valid=1; rw_mask_on=1; rw_tee=1;
   rw_distrust=1; rw_reset_owed=0; rw_rlog_miss_seen=0;
   gba_rewind_rlog_miss=0; rw_rlog_bytes=131072;
   memset(battery,0x5a,sizeof(battery));
}
static void check_reset(int before)
{
   unsigned i;
   assert(reset_count==before+1);
   assert(!rw_gesture && !rw_present_valid && !rw_mask_on && !rw_tee);
   assert(!rw_reset_owed && !rw_distrust);
   for(i=0;i<sizeof(battery);i++) assert(battery[i]==0x5a);
}
int main(void)
{
   setup(); injected_result=FE_RW_ERROR;
   assert(fe_host_rewind_tick_step(0,1)==FE_RW_PLAY);
   check_reset(0);
   /* An unlogged save read is reported, not a reset (4.1.1). */
   setup(); injected_result=FE_RW_SHOW; gba_rewind_rlog_miss=1;
   assert(fe_host_rewind_tick_step(0,1)==FE_RW_SHOW);
   assert(reset_count==1 && rw_rlog_miss_seen==1 && rw_gesture);
   setup(); injected_result=FE_RW_PLAY;
   assert(fe_host_rewind_tick_step(0,1)==FE_RW_PLAY);
   assert(reset_count==1);
   puts("frontend replay faults: failed restore resets before PLAY, battery "
        "retained; unlogged save read reported only");
   return 0;
}

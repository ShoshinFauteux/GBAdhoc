/* Behavioural test of the autopilot ENGINE's rig-facing output: the per-line
 * identity the double-battle rig scores on (docs/RIG-DOUBLE-BATTLE.md §2).
 *
 *   - ap_loaded carries crc= (CRC32 of the script bytes);
 *   - it= is the 1-based repeat iteration on ap_mark/ap_sync/ap_val, 0 outside;
 *   - ap_fail carries the last value the failing predicate read;
 *   - logbytes emits the exact bytes as hex.
 *
 * The real fe_autopilot.c is compiled in; fe_evt and the host services are
 * replaced by recorders (functions, not headers, like test_autopilot_script).
 */
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_lines[4096][160];
static int  g_n;

void fe_log(const char *fmt, ...) { (void)fmt; }
void fe_evt(const char *fmt, ...)
{
   va_list ap;
   if (g_n >= 4096) return;
   va_start(ap, fmt);
   vsnprintf(g_lines[g_n++], sizeof(g_lines[0]), fmt, ap);
   va_end(ap);
}
unsigned long long fe_evt_now_us(void) { return 1234000ull; }

static uint8_t g_ram[64];
unsigned fe_host_frame_count(void) { return 0; }
static uint32_t g_inj[256];
static int g_ninj;
void fe_host_input_inject(uint32_t mask) { if (g_ninj < 256) g_inj[g_ninj++] = mask; }
int fe_host_mem_read(uint32_t addr, void *out, unsigned len)
{
   if (addr < 0x02000000u || addr + len > 0x02000000u + sizeof(g_ram))
      return -1;
   memcpy(out, g_ram + (addr - 0x02000000u), len);
   return 0;
}
uint32_t fe_host_sram_crc_now(void) { return 0; }

#include "../../frontend-common/fe_autopilot.c"

static const char *find(const char *prefix, int nth)
{
   int i, k = 0;
   for (i = 0; i < g_n; i++)
      if (!strncmp(g_lines[i], prefix, strlen(prefix)) && k++ == nth)
         return g_lines[i];
   return NULL;
}

static void run_frames(int n)
{
   while (n-- && fe_autopilot_status() == 0)
      fe_autopilot_frame();
}

int main(void)
{
   const char *path = "/tmp/t_ap_engine.inputs";
   const char *script =
      "evt start\n"
      "repeat 3\n"
      "waitram 1 0x02000000 0xFF 0 5\n"
      "logbytes fp 4 0x02000010\n"
      "evt turn\n"
      "endrepeat\n"
      "evt after\n"
      "waitram 1 0x02000001 0xFF 0x77 3\n";
   FILE *f = fopen(path, "w");
   const char *l;
   char want[64];
   int i;

   assert(f);
   fputs(script, f);
   fclose(f);
   memset(g_ram, 0, sizeof(g_ram));
   g_ram[1] = 0x42;
   g_ram[0x10] = 0xde; g_ram[0x11] = 0xad; g_ram[0x12] = 0xbe; g_ram[0x13] = 0xef;

   assert(fe_autopilot_load(path) == 0);
   l = find("ap_loaded", 0);
   snprintf(want, sizeof(want), "crc=%08x",
            (unsigned)ap_crc32(0, script, strlen(script)));
   assert(l && strstr(l, want));

   run_frames(100);
   assert(fe_autopilot_status() == -1);            /* last waitram fails */

   assert((l = find("ap_mark text=start", 0)) && strstr(l, " it=0"));
   for (i = 0; i < 3; i++)
   {
      snprintf(want, sizeof(want), " it=%d", i + 1);
      assert((l = find("ap_mark text=turn", i)) && strstr(l, want));
      assert((l = find("ap_val name=fp", i)) && strstr(l, "hex=deadbeef") &&
             strstr(l, want));
      assert((l = find("ap_sync", i)) && strstr(l, want) && strstr(l, "t_ms=1234"));
   }
   assert(!find("ap_mark text=turn", 3));
   assert((l = find("ap_mark text=after", 0)) && strstr(l, " it=0"));
   assert((l = find("ap_fail", 0)) && strstr(l, "val=0x00000042") &&
          strstr(l, "op=waitram"));
   /* mashif: A only while the condition byte is 1, stop when byte 3 == 9 */
   {
      const char *p2 = "/tmp/t_ap_mashif.inputs";
      int k, pressed_while_off = 0, pressed_while_on = 0;
      f = fopen(p2, "w");
      assert(f);
      fputs("mashif A 1 0x02000002 0xFF 1 1 0x02000003 0xFF 9 100\n", f);
      fclose(f);
      memset(g_ram, 0, sizeof(g_ram));
      assert(fe_autopilot_load(p2) == 0);
      for (k = 0; k < 40; k++)
      {
         int on = (k >= 10 && k < 30);
         g_ram[2] = (uint8_t)on;
         g_ninj = 0;
         fe_autopilot_frame();
         if (g_ninj && (g_inj[0] & 0x100))
         {
            if (on) pressed_while_on++;
            else pressed_while_off++;
         }
      }
      g_ram[3] = 9;
      fe_autopilot_frame();
      fe_autopilot_frame();
      assert(fe_autopilot_status() == 1);
      assert(pressed_while_off == 0 && pressed_while_on >= 4);
   }
   /* Every shipped rig fixture must PARSE with this engine: a fixture is
    * staged onto the consoles between hardware runs, and a parse error there
    * costs a whole run (the H0 position probe was added mid-campaign). */
   {
      static const char *fx[] = {
         "testdata/fixtures/frlg_battle_host.inputs",
         "testdata/fixtures/frlg_battle_join.inputs",
         "testdata/fixtures/emerald_battle_host.inputs",
         "testdata/fixtures/emerald_battle_join.inputs" };
      unsigned i;
      int parsed = 0;
      for (i = 0; i < sizeof(fx) / sizeof(fx[0]); i++)
      {
         FILE *t = fopen(fx[i], "r");
         if (!t)
            continue;               /* run from another directory */
         fclose(t);
         if (fe_autopilot_load(fx[i]) != 0)
         {
            fprintf(stderr, "fixture does not parse: %s\n", fx[i]);
            return 1;
         }
         parsed++;
      }
      printf("rig fixtures parsed: %d\n", parsed);
   }
   printf("ap engine: crc, it=, logbytes, fail val, mashif all as specified\n");
   return 0;
}

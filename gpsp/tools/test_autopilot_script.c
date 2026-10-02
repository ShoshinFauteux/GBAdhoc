/* test_autopilot_script.c — parse a real autopilot script with the REAL parser.
 *
 * WHY.  An input script is a text file that nothing checks until a console
 * reads it, and the parser rejects a bad line by logging and giving up.  On
 * hardware that looks exactly like a console that ran the job and did nothing:
 * the window closes, the run is empty, and the fixture gets blamed.  The soak
 * script is 90 iterations long and the whole point of it is to run unattended
 * for ten minutes, so "it silently did nothing" is the expensive failure.
 *
 * This compiles fe_autopilot.c itself rather than restating its rules, because
 * a second copy of the grammar is a second thing to keep in step -- and the
 * rules that actually bite are not obvious ones: AP_MAX_STEPS is 256 in a
 * normal build and 1024 in GPSP_PERF_RIG; it counts the FILE's steps (repeat is
 * a runtime loop, so iterations are free), repeat
 * cannot nest, and a run of zero-frame steps longer than AP_MAX_CHAIN fails
 * when executed (it is not rejected by the parser).
 *
 *   gcc -std=gnu99 -w -DGPSP_PERF_RIG -Ifrontend-common -o /tmp/t_ap tools/test_autopilot_script.c
 *   /tmp/t_ap testdata/fixtures/emerald_soak.inputs
 *
 * Exits non-zero if the parser rejects the script.
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* --- the frontend services fe_autopilot.c expects -------------------------
 * Replace the FUNCTIONS, not the headers: the real fe_autopilot.h and the real
 * fe_evt.h are included below so the test cannot drift from the declarations
 * the parser is actually compiled against. */
static int g_verbose;

void fe_log(const char *fmt, ...)
{
   va_list ap;
   fputs("  parser: ", stdout);
   va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
   fputc('\n', stdout);
}

/* fe_evt is a no-op here; the script's evt marks are output, not input. */
void fe_evt(const char *fmt, ...) { (void)fmt; }
unsigned long long fe_evt_now_us(void) { return 0; }

unsigned fe_host_frame_count(void) { return 0; }
void fe_host_input_inject(uint32_t mask) { (void)mask; }
int  fe_host_mem_read(uint32_t addr, void *out, unsigned len)
{ (void)addr; (void)out; (void)len; return 0; }
uint32_t fe_host_sram_crc_now(void) { return 0; }

#include "fe_autopilot.c"

int main(int argc, char **argv)
{
   const char *path;
   int i;

   if (argc < 2)
   {
      fprintf(stderr, "usage: %s <script.inputs> [--verbose]\n", argv[0]);
      return 2;
   }
   path = argv[1];
   for (i = 2; i < argc; i++)
      if (!strcmp(argv[i], "--verbose")) g_verbose = 1;

   printf("parsing %s\n", path);
   if (fe_autopilot_load(path) != 0)
   {
      printf("REJECTED -- a console would log the reason above and run no "
             "input at all\n");
      return 1;
   }

   printf("  accepted: %d steps of %d (%d%% of the file limit)\n",
          step_count, AP_MAX_STEPS, 100 * step_count / AP_MAX_STEPS);

   /* Report the loop structure, because the step count alone hides it: a
    * `repeat 90` body of 20 steps is 20 steps in the file and 1800 executed. */
   {
      int repeats = 0, body = 0, in_body = 0;
      long executed = 0;
      for (i = 0; i < step_count; i++)
      {
         if (steps[i].op == OP_REPEAT) { repeats++; in_body = 1; body = 0; continue; }
         if (steps[i].op == OP_ENDREPEAT)
         {
            in_body = 0;
            printf("  repeat block: %d steps x %u iterations = %ld executed\n",
                   body, steps[i - body - 1].off,
                   (long)body * (long)steps[i - body - 1].off);
            executed += (long)body * (long)steps[i - body - 1].off;
            continue;
         }
         if (in_body) body++; else executed++;
      }
      printf("  %d repeat block(s), ~%ld steps executed per run\n",
             repeats, executed);
   }

   /* Frame span. Fixed input steps have an exact duration. Predicate steps
    * (waitram/mash/holdram/waitsram) consume a variable number: they take at
    * least one frame if already satisfied, and at most timeout+1 frames
    * (timeout failed checks plus the final check/failure frame). A repeat
    * multiplies both bounds. A perf_to beyond the minimum is not guaranteed to
    * be reached, so the collector can otherwise reject a valid script run for
    * having an incomplete measurement window. */
   {
      unsigned long long min_total = 0, max_total = 0;
      int in_body = 0;
      unsigned long long body_min = 0, body_max = 0;
      unsigned iters = 0;
      for (i = 0; i < step_count; i++)
      {
         unsigned long long fmin = 0, fmax = 0;
         switch (steps[i].op)
         {
            case OP_WAIT:
            case OP_HOLD:
               fmin = fmax = steps[i].frames; break;
            case OP_PRESS:
               fmin = fmax = (unsigned long long)steps[i].frames + AP_PRESS_GAP;
               break;
            case OP_WAITRAM:
            case OP_MASH:
            case OP_HOLDRAM:
            case OP_WAITSRAM:
            case OP_MASHIF:
            case OP_STEPRAM:
               fmin = 1;
               fmax = (unsigned long long)steps[i].frames + 1;
               break;
            case OP_REPEAT:
               in_body = 1; body_min = body_max = 0;
               iters = steps[i].off; continue;
            case OP_ENDREPEAT:
               in_body = 0;
               min_total += body_min * iters;
               max_total += body_max * iters;
               continue;
            default: break;   /* evt, ff, dump, state cost no frames */
         }
         if (in_body) { body_min += fmin; body_max += fmax; }
         else { min_total += fmin; max_total += fmax; }
      }
      if (min_total == max_total)
         printf("  script input duration: %llu frames (fixed)\n", min_total);
      else
         printf("  script input duration: %llu..%llu frames (predicate-dependent)\n",
                min_total, max_total);
      printf("  predicate semantics: success consumes 1..timeout+1 frames; "
             "an unsatisfied predicate fails on timeout+1\n");
      printf("  perf_to must fit within the guaranteed script span (minimum %llu "
             "frames); add frontend startup/state-load frames separately\n",
             min_total);
   }

   if (g_verbose)
      for (i = 0; i < step_count; i++)
         printf("    %3d  %-10s frames=%u buttons=%08x off=%u\n", i,
                op_name[steps[i].op], steps[i].frames,
                (unsigned)steps[i].buttons, steps[i].off);

   printf("OK\n");
   return 0;
}

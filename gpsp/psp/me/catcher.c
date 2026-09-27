/* catcher.c -- ME_CATCH=1 only: a CPU-exception screen for the hard freeze.
 *
 * WHY IT LIVES IN THE ME PRX.  The Go / 3000 hard freeze (frozen picture,
 * HOME dead, power-off needed) has only been seen on release builds, and every
 * instrumented build so far also moved the EBOOT's code and data around.  This
 * PRX is a separately loaded KERNEL module, so it can register the kernel's
 * default exception handler -- which a user-mode EBOOT cannot -- WITHOUT
 * changing a byte of the EBOOT.  A release EBOOT paired with this PRX has the
 * release code layout exactly.
 *
 * HOW.  catcher_vec.S is the registered handler.  It follows the PSPSDK's own
 * _pspDebugExceptionHandler entry protocol (disassembled from libpspdebug:
 * two leading NOPs, the original v0/v1 in COP0 control registers $4/$5, EPC /
 * BadVAddr / Cause / Status in COP0 $14 / $8 / $13 / $12), saves the GPRs,
 * switches to a private stack and calls catch_main() -- still IN EXCEPTION
 * CONTEXT, kernel mode, interrupts off.  catch_main draws straight into the
 * three VRAM framebuffers the frontend uses (video_psp.c: 512-pixel stride,
 * 5650, VID_TRIPLE offsets 0 / 0x44000 / 0x88000), then spins forever.  No
 * syscall, no import beyond the registration itself: nothing here can depend
 * on a kernel state the exception has left broken, and no NID can fail to
 * resolve and take the Media Engine down with it.
 *
 * READING THE SCREEN.
 *   EPC inside the EBOOT .text (0x0880xxxx, see gpsp_adhoc.elf) -> a C/asm bug;
 *   EPC inside rom_/ram_translation_cache                       -> the dynarec
 *       executed bad native code (stale icache, garbage patch).  On a
 *       PSP-1000 (small tier) that is the EBOOT .bss; on a 64 MiB console
 *       (large tier) it is ONE HEAP BLOCK of 10.5 MiB, typically ~0x08Fxxxxx
 *       -- `EVT jit_cache ... base=` in a harness log gives the exact start;
 *   EPC == 0x08000000 or near 0x0FFFFFFC                         -> an unpatched
 *       j-filler or sentinel jump;
 *   NO screen on a hard freeze -> not a CPU exception: a GE hang, a kernel
 *       deadlock, or a bus / Media Engine wedge.
 *   "ME beat a -> b" with a == b: the Media Engine was not running.
 *
 * Costs nothing until an exception: one registration at module_start. */
#ifdef ME_CATCH
#include <pspkernel.h>
#include <pspexception.h>

#include "me_mbox.h"
#include "catcher_font.h"

/* In libpspkernel.a (ExceptionManagerForKernel) but not in pspexception.h. */
int sceKernelReleaseDefaultExceptionHandler(void *func);

extern void me_catch_vector(void);

/* Filled by catcher_vec.S: r[0..31], then badvaddr, cause, epc, status. */
unsigned int g_catch_regs[36];
unsigned int g_catch_stack[1024] __attribute__((aligned(16)));

static volatile me_mbox *g_catch_mb;
static int g_catch_installed;

#define CATCH_VRAM     0x44000000u      /* uncached VRAM */
#define CATCH_STRIDE   512
#define CATCH_FB_BYTES (512u * 272u * 2u)
#define CATCH_BG       0x5000u          /* 5650, dark blue (B in the high bits) */
#define CATCH_FG       0xFFFFu

static int cx, cy;

static void catch_putc(int c)
{
   int fb, row, col;
   if (c == '\n' || cx >= 60)
   {
      cx = 0;
      cy++;
      if (c == '\n')
         return;
   }
   if (cy >= 34)
      return;
   if (c < 0x20 || c > 0x7F)
      c = '?';
   for (fb = 0; fb < 3; fb++)
   {
      volatile unsigned short *p = (volatile unsigned short *)
         (CATCH_VRAM + fb * CATCH_FB_BYTES) + cy * 8 * CATCH_STRIDE + cx * 8;
      for (row = 0; row < 8; row++)
      {
         unsigned bits = catch_font[(c - 0x20) * 8 + row];
         for (col = 0; col < 8; col++)
            p[row * CATCH_STRIDE + col] = (bits & (0x80u >> col)) ? CATCH_FG
                                                                  : CATCH_BG;
      }
   }
   cx++;
}

static void catch_puts(const char *s)
{
   while (*s)
      catch_putc(*s++);
}

static void catch_hex(unsigned int v)
{
   int i;
   for (i = 28; i >= 0; i -= 4)
      catch_putc("0123456789ABCDEF"[(v >> i) & 0xF]);
}

static void catch_dec(unsigned int v)
{
   char b[12];
   int n = 0;
   do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 11);
   while (n)
      catch_putc(b[--n]);
}

static int catch_readable(unsigned int a)
{
   a &= 0x1FFFFFFFu;
   return (a & 3) == 0 && a >= 0x08000000u + 64 && a < 0x0C000000u - 64;
}

static void catch_words(const char *what, unsigned int a, int before, int n)
{
   int i;
   catch_puts(what);
   catch_hex(a);
   catch_putc(':');
   if (!catch_readable(a))
   {
      catch_puts(" not RAM\n");
      return;
   }
   for (i = -before; i < n - before; i++)
   {
      catch_putc(i ? ' ' : '>');
      catch_hex(*(volatile unsigned int *)
                (((a & 0x1FFFFFFFu) | 0x80000000u) + 4 * i));
   }
   catch_putc('\n');
}

static const char *const catch_exc[32] = {
   "Int", "Mod", "TLBL", "TLBS", "AdEL (load addr)", "AdES (store addr)",
   "IBE (fetch bus err)", "DBE (data bus err)", "Syscall", "Break",
   "RI (bad instruction)", "CpU", "Ov", "Trap", "?", "FPE",
   "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?",
   "?"
};

static const char *const catch_reg[32] = {
   "zr", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3",
   "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
   "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"
};

void catch_main(void)
{
   unsigned int *r = g_catch_regs;
   unsigned int b0 = 0, b1 = 0, i;
   int fb;

   for (fb = 0; fb < 3; fb++)
   {
      volatile unsigned int *p =
         (volatile unsigned int *)(CATCH_VRAM + fb * CATCH_FB_BYTES);
      for (i = 0; i < CATCH_FB_BYTES / 4; i++)
         p[i] = CATCH_BG | (CATCH_BG << 16);
   }
   cx = cy = 0;
   catch_puts("GBAdhoc CATCH: the PSP crashed. Photograph this,\n");
   catch_puts("then hold POWER to switch off.\n\n");
   catch_puts("Exception ");
   catch_dec((r[33] >> 2) & 31);
   catch_putc(' ');
   catch_puts(catch_exc[(r[33] >> 2) & 31]);
   catch_puts("\nEPC ");    catch_hex(r[34]);
   catch_puts("  BadVA ");  catch_hex(r[32]);
   catch_puts("\nCause ");  catch_hex(r[33]);
   catch_puts("  Status "); catch_hex(r[35]);
   catch_putc('\n');
   for (i = 0; i < 32; i++)
   {
      catch_puts(catch_reg[i]);
      catch_putc(' ');
      catch_hex(r[i]);
      catch_puts((i & 3) == 3 ? "\n" : "  ");
   }
   catch_words("EPC ", r[34], 4, 8);
   catch_words("RA  ", r[31], 4, 6);
   catch_words("SP  ", r[29], 0, 6);
   catch_words("SP+ ", r[29] + 24, 0, 6);
   if (g_catch_mb)
   {
      volatile int spin;
      b0 = g_catch_mb->heartbeat;
      for (spin = 0; spin < 2000000; spin++)
         ;
      b1 = g_catch_mb->heartbeat;
      catch_puts("ME magic ");  catch_hex(g_catch_mb->magic);
      catch_puts(" beat ");     catch_dec(b0);
      catch_puts(" -> ");       catch_dec(b1);
      catch_puts("\n   cmd ");  catch_dec(g_catch_mb->cmd_seq);
      catch_puts(" done ");     catch_dec(g_catch_mb->done_seq);
      catch_puts(" in ");       catch_dec(g_catch_mb->input_seq);
      catch_putc('\n');
   }
   for (;;)
      ;                                  /* hold the picture */
}

static void me_catch_install(volatile me_mbox *mb)
{
   int rc;
   g_catch_mb = mb;
#ifdef ME_CATCH_NOREG
   /* A/B arm: identical code size and imports, but nothing is registered
    * (releasing a handler that was never registered just returns an error). */
   rc = sceKernelReleaseDefaultExceptionHandler(
           (void *)((unsigned int)me_catch_vector | 0x80000000u));
#else
   rc = sceKernelRegisterDefaultExceptionHandler(
           (void *)((unsigned int)me_catch_vector | 0x80000000u));
#endif
   g_catch_installed = (rc >= 0);
   /* Say what happened where the host can read it (the field was reserved
    * for exception bookkeeping): 0xCA7C0000 = installed, else the error. */
   mb->me_faults = g_catch_installed ? 0xCA7C0000u : (unsigned int)rc;
}

static void me_catch_remove(void)
{
   if (g_catch_installed)
      sceKernelReleaseDefaultExceptionHandler(
         (void *)((unsigned int)me_catch_vector | 0x80000000u));
   g_catch_installed = 0;
}

/* The exported entry points (exports.exp names module_start/module_stop);
 * main.c's own are renamed to *_inner under ME_CATCH. */
int me_module_start_inner(SceSize args, void *argp);
int me_module_stop_inner(SceSize args, void *argp);

int module_start(SceSize args, void *argp)
{
   int rc = me_module_start_inner(args, argp);
   if (rc >= 0 && args >= 4 && argp && *(unsigned int *)argp)
      me_catch_install((volatile me_mbox *)(*(unsigned int *)argp));
   return rc;
}

int module_stop(SceSize args, void *argp)
{
   me_catch_remove();        /* before the module's memory can go away */
   return me_module_stop_inner(args, argp);
}
#endif

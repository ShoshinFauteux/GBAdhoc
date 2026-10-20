/* lab_harness.c -- the GBAdhoc Lab task runner.  LAB_HARNESS=1 only.
 *
 * The protocol is GBAdhoc-Lab docs/PROTOCOL.md; section numbers below refer
 * to it.  The rules that matter most, because each one was paid for:
 *
 *  - ONE WINDOW PER PROCESS (H1).  After the USB window the process touches
 *    ms0 no more and relaunches itself: a PSP that keeps a FAT cache across
 *    a window in which the PC changed the card writes stale chains back
 *    (go-m2-handoff-fat-damage).  Every read of the PC's files happens at
 *    the next boot, after LoadExec, with a fresh FAT.
 *  - SINGLE WRITER per path (H2): this file only writes LAB.ID, sys/ and
 *    out/; it deletes only out/ entries the PC acknowledged.
 *  - NOTHING while exported: lab_ms0_ok() gates every file operation.
 *  - TELEMETRY NEVER BLOCKS (H5): a static ring filled by the main loop,
 *    written synchronously only at task end or every lab_tlm_flush_s.
 *
 * The USB bring-up/teardown is a deliberate COPY of usb_handoff.c's proven
 * sequence: that file stays byte-for-byte untouched (owner, 2026-09-30).
 */
#ifdef LAB_HARNESS
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>

#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <pspusb.h>
#include <pspusbstor.h>
#include <psppower.h>
#include <pspctrl.h>
#include <psputils.h>
#include <kubridge.h>
#include <psploadexec_kernel.h>
#include <systemctrl.h>

#include "lab_harness.h"
#include "fe_util.h"
#include "fe_evt.h"

/* ------------------------------------------------------------- counters */
extern uint32_t flush_rom_total, gamepak_reopens, gamepak_short_reads, badjump_contained;
extern uint32_t dma_smc_flushes __attribute__((weak));
extern uint32_t badjump_recoveries __attribute__((weak));
extern uint32_t jit_code_stats[10] __attribute__((weak));

/* ---------------------------------------------------------------- state */
#define ROOT_MAX 32
static char L_root[ROOT_MAX];          /* "ms0:/GBALAB" */
static char L_self[160];               /* this EBOOT's path */
static char L_self_bid[16];            /* "" = the launcher */
static char L_tid[48], L_ini[192], L_out[128], L_work[160], L_bid[16], L_fixture[48];
static char L_state_in[160], L_sav_in[160];
static int  L_active, L_usb_active;
static unsigned L_attempt = 1, L_eboot_crc, L_model;
static int  L_tlm_every = 300, L_tlm_flush_s = 60, L_window_after = 1, L_window_s = 30;
static int  L_wall_limit_s = 900, L_attempts = 1;
static uint64_t L_t0_us, L_last_flush_us;
static int  L_batt_start = -1;
static unsigned L_last_frame;

typedef struct {
   unsigned boot, hop, cmd_seq, idle_s, next_window_s;
   char last_token[64], mode[8];
} lab_bootf;
static lab_bootf L_b;

/* ------------------------------------------------------------ file utils */
static int lab_ms0_ok(void)
{
   return !L_usb_active;   /* THE gate: never touch the card while exported */
}

__attribute__((noinline)) static void path2(char *out, size_t n, const char *a, const char *b)
{
   snprintf(out, n, "%s/%s", a, b);
}

static int rd(const char *p, char *buf, int cap)
{
   int fd, n;
   if (!lab_ms0_ok())
      return -1;
   fd = sceIoOpen(p, PSP_O_RDONLY, 0777);
   if (fd < 0)
      return -1;
   n = sceIoRead(fd, buf, cap - 1);
   sceIoClose(fd);
   if (n < 0)
      n = 0;
   buf[n] = 0;
   return n;
}

static int wr(const char *p, const void *d, int n, int append)
{
   int fd, w;
   if (!lab_ms0_ok())
      return -1;
   fd = sceIoOpen(p, PSP_O_WRONLY | PSP_O_CREAT | (append ? PSP_O_APPEND : PSP_O_TRUNC), 0777);
   if (fd < 0)
      return -1;
   w = sceIoWrite(fd, d, n);
   sceIoClose(fd);
   return w == n ? 0 : -1;
}

static int fsize(const char *p)
{
   SceIoStat st;
   if (!lab_ms0_ok())
      return -1;
   memset(&st, 0, sizeof(st));
   if (sceIoGetstat(p, &st) < 0)
      return -1;
   return (int)st.st_size;
}

static int exists(const char *p)
{
   return fsize(p) >= 0;
}

static int copy_file(const char *src, const char *dst)
{
   static char buf[16384];
   int in, out, n, rc = 0;
   if (!lab_ms0_ok())
      return -1;
   in = sceIoOpen(src, PSP_O_RDONLY, 0777);
   if (in < 0)
      return -1;
   out = sceIoOpen(dst, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
   if (out < 0)
   {
      sceIoClose(in);
      return -1;
   }
   while ((n = sceIoRead(in, buf, sizeof(buf))) > 0)
      if (sceIoWrite(out, buf, n) != n)
      {
         rc = -1;
         break;
      }
   sceIoClose(in);
   sceIoClose(out);
   return rc;
}

/* Remove a directory tree we own (out/<tid>/ and its work/).  Depth-capped. */
static void rmtree(const char *dir, int depth)
{
   SceIoDirent de;
   SceUID d;
   char p[192];
   if (!lab_ms0_ok() || depth > 3)
      return;
   d = sceIoDopen(dir);
   if (d >= 0)
   {
      memset(&de, 0, sizeof(de));
      while (sceIoDread(d, &de) > 0)
      {
         if (de.d_name[0] && strcmp(de.d_name, ".") && strcmp(de.d_name, ".."))
         {
            path2(p, sizeof(p), dir, de.d_name);
            if (FIO_S_ISDIR(de.d_stat.st_mode))
               rmtree(p, depth + 1);
            else
               sceIoRemove(p);
         }
         memset(&de, 0, sizeof(de));
      }
      sceIoDclose(d);
   }
   sceIoRmdir(dir);
}

static uint32_t crc32_buf(uint32_t c, const unsigned char *p, int n)
{
   int k;
   c = ~c;
   while (n-- > 0)
   {
      c ^= *p++;
      for (k = 0; k < 8; k++)
         c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
   }
   return ~c;
}

/* key=value lookup in a small text buffer (BOOT/CMD/LAB.ID/CURRENT/ACK). */
static int kv(const char *text, const char *key, char *out, int cap)
{
   size_t kl = strlen(key);
   const char *p = text;
   while (p && *p)
   {
      while (*p == '\r' || *p == '\n' || *p == ' ')
         p++;
      if (!strncmp(p, key, kl) && p[kl] == '=')
      {
         int i = 0;
         p += kl + 1;
         while (*p && *p != '\r' && *p != '\n' && i < cap - 1)
            out[i++] = *p++;
         out[i] = 0;
         return 1;
      }
      p = strchr(p, '\n');
   }
   out[0] = 0;
   return 0;
}

static unsigned kvu(const char *text, const char *key, unsigned def)
{
   char v[24];
   return kv(text, key, v, sizeof(v)) && v[0] ? (unsigned)strtoul(v, NULL, 0) : def;
}

static void journal(const char *fmt, ...)
{
   char line[200], p[64];
   va_list ap;
   int n = snprintf(line, sizeof(line), "b%u t%u ", L_b.boot, (unsigned)sceKernelGetSystemTimeLow());
   va_start(ap, fmt);
   n += vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
   va_end(ap);
   if (n > (int)sizeof(line) - 2)
      n = (int)sizeof(line) - 2;
   line[n++] = '\n';
   path2(p, sizeof(p), L_root, "sys/JOURNAL.TXT");
   if (fsize(p) > 65536)
   {
      char old[64];
      path2(old, sizeof(old), L_root, "sys/JOURNAL.OLD");
      sceIoRemove(old);
      sceIoRename(p, old);
   }
   wr(p, line, n, 1);
}

/* ---------------------------------------------------------- BOOT.TXT etc */
static void boot_load(void)
{
   char buf[512], p[64];
   path2(p, sizeof(p), L_root, "sys/BOOT.TXT");
   if (rd(p, buf, sizeof(buf)) < 0)
      buf[0] = 0;
   L_b.boot = kvu(buf, "boot", 0);
   L_b.hop = kvu(buf, "hop", 0);
   L_b.cmd_seq = kvu(buf, "cmd_seq", 0);
   L_b.idle_s = kvu(buf, "idle_s", 30);
   L_b.next_window_s = kvu(buf, "next_window_s", 30);
   kv(buf, "last_token", L_b.last_token, sizeof(L_b.last_token));
   if (!kv(buf, "mode", L_b.mode, sizeof(L_b.mode)) || !L_b.mode[0])
      strcpy(L_b.mode, "RUN");
}

static void boot_save(void)
{
   char buf[320], p[64];
   int n = snprintf(buf, sizeof(buf), "v=1\nboot=%u\nhop=%u\nlast_token=%s\ncmd_seq=%u\nmode=%s\n"
                    "idle_s=%u\nnext_window_s=%u\nharness_v=%d\n", L_b.boot, L_b.hop, L_b.last_token,
                    L_b.cmd_seq, L_b.mode, L_b.idle_s, L_b.next_window_s, LAB_HARNESS_V);
   path2(p, sizeof(p), L_root, "sys/BOOT.TXT");
   wr(p, buf, n, 0);
}

static char L_labid[20];

static void labid_ensure(void)
{
   char buf[256], p[64];
   path2(p, sizeof(p), L_root, "LAB.ID");
   if (rd(p, buf, sizeof(buf)) > 0 && kv(buf, "id", L_labid, sizeof(L_labid)) && strlen(L_labid) == 16)
      return;
   {
      SceKernelUtilsMt19937Context ctx;
      unsigned hi, lo;
      int n;
      static const char *names[] = { "PSP-1000", "PSP-2000", "PSP-3000", "PSP-3000", "PSP-Go" };
      sceKernelUtilsMt19937Init(&ctx, (u32)(sceKernelGetSystemTimeWide() ^ (L_model << 24) ^ 0x6C616221u));
      hi = sceKernelUtilsMt19937UInt(&ctx);
      lo = sceKernelUtilsMt19937UInt(&ctx) ^ (unsigned)sceKernelGetSystemTimeLow();
      snprintf(L_labid, sizeof(L_labid), "%08x%08x", hi, lo);
      n = snprintf(buf, sizeof(buf), "v=1\nid=%s\nmodel=%u\nmodel_name=%s\ncreated_boot=%u\n", L_labid, L_model,
                   L_model < 5 ? names[L_model] : "unknown", L_b.boot);
      wr(p, buf, n, 0);
   }
}

/* ----------------------------------------------------------------- USB */
#define LAB_USB_PID 0x1c8
static const char *const lab_usb_prx[] = {
   "flash0:/kd/semawm.prx", "flash0:/kd/usbstor.prx", "flash0:/kd/usbstormgr.prx",
   "flash0:/kd/usbstorms.prx", "flash0:/kd/usbstorboot.prx",
};

static int lab_usb_up(void)
{
   unsigned i;
   int rc;
   for (i = 0; i < sizeof(lab_usb_prx) / sizeof(lab_usb_prx[0]); i++)
   {
      SceUID id = kuKernelLoadModule(lab_usb_prx[i], 0, NULL);
      int st = 0;
      if (id >= 0)
         sceKernelStartModule(id, 0, NULL, &st, NULL);
   }
   if (sceUsbStart(PSP_USBBUS_DRIVERNAME, 0, 0) < 0)
      return -2;
   if (sceUsbStart(PSP_USBSTOR_DRIVERNAME, 0, 0) < 0)
      return -3;
   sceUsbstorBootSetCapacity(8 * 1024 * 1024);
   rc = sceUsbActivate(LAB_USB_PID);
   if (rc < 0)
      return -5;
   L_usb_active = 1;
   return 0;
}

static int lab_usb_down(void)
{
   int rc = sceUsbDeactivate(LAB_USB_PID);
   if (L_usb_active && rc < 0)
      return -1;     /* the PC may still own the volume: stay gated */
   L_usb_active = 0;
   sceUsbStop(PSP_USBSTOR_DRIVERNAME, 0, 0);
   sceUsbStop(PSP_USBBUS_DRIVERNAME, 0, 0);
   return 0;
}

/* ------------------------------------------------------------- relaunch */
static void lab_loadexec(const char *eboot)
{
   struct SceKernelLoadExecVSHParam param;
   memset(&param, 0, sizeof(param));
   param.size = sizeof(param);
   param.args = (SceSize)(strlen(eboot) + 1);
   param.argp = (void *)eboot;
   param.key  = "game";
   sctrlKernelLoadExecVSHMs2(eboot, &param);
   sceKernelLoadExec(eboot, NULL);
}

static void lab_relaunch_and_die(const char *eboot)
{
   char launcher[64];
   lab_loadexec(eboot);
   snprintf(launcher, sizeof(launcher), "%.4s/PSP/GAME/GBALAB/EBOOT.PBP", L_root);
   if (strcmp(launcher, eboot))
      lab_loadexec(launcher);
   sceKernelExitGame();
}

/* ------------------------------------------------------------- windows */
#define LAB_ESCAPE (PSP_CTRL_START | PSP_CTRL_SELECT)

static int batt_pct(void)
{
   int b = scePowerGetBatteryLifePercent();
   return (b < 0 || b > 100) ? -1 : b;
}

/* sec. 6.3.  Never returns. */
static void lab_window(const char *kind, unsigned seconds)
{
   char p[64], buf[1024], ready[640] = "";
   int n, ms = 0, esc = 0, rlen = 0, b;
   char qseq[16] = "0";
   {  /* ready = finished results still on the card */
      SceIoDirent de;
      SceUID d;
      char od[64], rp[160];
      path2(od, sizeof(od), L_root, "out");
      d = sceIoDopen(od);
      if (d >= 0)
      {
         memset(&de, 0, sizeof(de));
         while (sceIoDread(d, &de) > 0)
         {
            if (de.d_name[0] && de.d_name[0] != '.' && FIO_S_ISDIR(de.d_stat.st_mode) && strlen(de.d_name) <= 48)
            {
               snprintf(rp, sizeof(rp), "%s/%.48s/RESULT.TXT", od, de.d_name);
               if (exists(rp) && rlen + (int)strlen(de.d_name) + 2 < (int)sizeof(ready))
                  rlen += snprintf(ready + rlen, sizeof(ready) - rlen, "%s%s", rlen ? "," : "", de.d_name);
            }
            memset(&de, 0, sizeof(de));
         }
         sceIoDclose(d);
      }
   }
   path2(p, sizeof(p), L_root, "QUEUE.TXT");
   if (rd(p, buf, sizeof(buf)) > 0)
      kv(buf, "seq", qseq, sizeof(qseq));
   snprintf(L_b.last_token, sizeof(L_b.last_token), "%s-%u-1", L_labid, L_b.boot);
   b = batt_pct();
   {
      char bs[8] = "";     /* unknown battery: an empty value, never 0 */
      if (b >= 0)
         snprintf(bs, sizeof(bs), "%d", b);
      n = snprintf(buf, sizeof(buf), "v=1\nid=%s\nboot=%u\nwin=1\ntoken=%s\nstate=open\nseconds=%u\nkind=%s\n"
                   "self=%s\nqueue_seq=%s\nready=%s\nbatt=%s\n", L_labid, L_b.boot, L_b.last_token, seconds,
                   kind, L_self, qseq, ready, bs);
      if (n >= (int)sizeof(buf))
         n = (int)sizeof(buf) - 1;
   }
   boot_save();
   journal("window %s %us token=%s", kind, seconds, L_b.last_token);
   path2(p, sizeof(p), L_root, "sys/WINDOW.TXT");
   wr(p, buf, n, 0);
   /* ---- the card belongs to the PC from here until lab_usb_down ---- */
   if (lab_usb_up() < 0)
   {
      if (lab_usb_down() < 0)
         sceKernelExitGame();       /* ownership unknown: no further access */
      lab_relaunch_and_die(L_self); /* no USB this boot; try again fresh */
   }
   while (ms < (int)seconds * 1000)
   {
      SceCtrlData pd;
      sceCtrlPeekBufferPositive(&pd, 1);
      if ((pd.Buttons & LAB_ESCAPE) == LAB_ESCAPE)
      {
         if ((esc += 200) >= 1500)
         {
            lab_usb_down();
            sceKernelExitGame();    /* a person can always get out */
         }
      }
      else
         esc = 0;
      scePowerTick(PSP_POWER_TICK_SUSPEND);
      scePowerTick(PSP_POWER_TICK_DISPLAY);
      sceKernelDelayThread(200 * 1000);
      ms += 200;
   }
   if (lab_usb_down() < 0)
      sceKernelExitGame();
   /* sec. 6.3.4: no read, no write, no park.  Relaunch, unconditionally. */
   lab_relaunch_and_die(L_self);
}

/* -------------------------------------------------------------- results */
static void write_result(const char *tid, const char *bid, const char *outcome, int code, const char *reason,
                         unsigned frames, unsigned t_ms, unsigned recs, unsigned drops, uint32_t ini_crc)
{
   char p[160], d[128], buf[768];
   int n, be = batt_pct();
   snprintf(d, sizeof(d), "%s/out/%s", L_root, tid);
   sceIoMkdir(d, 0777);
   n = snprintf(buf, sizeof(buf),
                "v=1\ntask=%s\nid=%s\nboot=%u\nbuild=%s\neboot_crc=%08x\nattempt=%u\noutcome=%s\nexit=%d\n"
                "reason=%s\nframes=%u\nt_ms=%u\nevt_drop=%u\nbatt_start=%d\nbatt_end=%d\ntlm_records=%u\n"
                "tlm_dropped=%u\nharness_v=%d\nini_crc=%08x\nlab_fixture=%s\n"
                "keys=lab_v,lab_task,lab_build,lab_fixture,lab_files,lab_state,lab_sav,lab_attempts,"
                "lab_wall_limit_s,lab_tlm_every,lab_tlm_flush_s,lab_window_after,lab_window_s\n",
                tid, L_labid, L_b.boot, bid, L_eboot_crc, L_attempt, outcome, code, reason ? reason : "ok",
                frames, t_ms, fe_evt_drops(), L_batt_start, be, recs, drops, LAB_HARNESS_V, (unsigned)ini_crc,
                L_fixture);
   path2(p, sizeof(p), d, "RESULT.TXT");
   wr(p, buf, n, 0);
}

/* -------------------------------------------------------------- telemetry */
#define TLM_RING 256
typedef struct { unsigned char b[64]; } tlm_rec;
static tlm_rec  T_ring[TLM_RING];
static unsigned T_head, T_flushed, T_dropped, T_seq, T_written, T_flush_flag, T_header_done;
static struct {
   unsigned n, over, wall_max, work_max, hist[8];
   uint64_t wall, work;
} T_win;
static unsigned T_wins;

static void put16(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; }
static void put32(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static unsigned char *tlm_new(int type, unsigned frame)
{
   unsigned char *r;
   unsigned t_ms = (unsigned)((sceKernelGetSystemTimeWide() - L_t0_us) / 1000ull);
   if (T_head - T_flushed >= TLM_RING)
   {
      T_dropped++;
      return NULL;            /* never block, never overwrite unflushed data */
   }
   r = T_ring[T_head % TLM_RING].b;
   memset(r, 0, 64);
   r[0] = (unsigned char)type;
   r[1] = (unsigned char)T_flush_flag;
   put16(r + 2, T_seq++);
   put32(r + 4, frame);
   put32(r + 8, t_ms);
   T_head++;
   return r;
}

void lab_event(int code, unsigned arg, const char *text)
{
   unsigned char *r;
   if (!L_active)
      return;
   r = tlm_new(2, 0);
   if (!r)
      return;
   put16(r + 12, (unsigned)code);
   put32(r + 16, arg);
   if (text)
      memcpy(r + 20, text, strnlen(text, 43));
}

static void tlm_header(void)
{
   unsigned char h[32];
   char p[160];
   unsigned long long id = strtoull(L_labid, NULL, 16);
   memset(h, 0, sizeof(h));
   memcpy(h, "GLT1", 4);
   put16(h + 4, 1);
   put16(h + 6, 64);
   put32(h + 8, (unsigned)(id >> 32));
   put32(h + 12, (unsigned)id);
   put32(h + 16, L_b.boot);
   put32(h + 20, crc32_buf(0, (const unsigned char *)L_tid, (int)strlen(L_tid)));
   put32(h + 24, L_eboot_crc);
   put16(h + 28, (unsigned)L_tlm_every);
   h[30] = (unsigned char)L_model;
   h[31] = 0;
   path2(p, sizeof(p), L_out, "TLM.BIN");
   wr(p, h, sizeof(h), 0);
}

static void tlm_flush(void)
{
   char p[160];
   int fd;
   if (!L_active || T_head == T_flushed || !lab_ms0_ok())
      return;
   if (!T_header_done)
   {
      tlm_header();
      T_header_done = 1;
   }
   path2(p, sizeof(p), L_out, "TLM.BIN");
   fd = sceIoOpen(p, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
   if (fd < 0)
      return;
   while (T_flushed != T_head)
   {
      unsigned i = T_flushed % TLM_RING, cnt = T_head - T_flushed;
      if (cnt > TLM_RING - i)
         cnt = TLM_RING - i;     /* contiguous part of the ring */
      if (sceIoWrite(fd, T_ring[i].b, (SceSize)(cnt * 64)) != (int)(cnt * 64))
         break;
      T_flushed += cnt;
      T_written += cnt;
   }
   sceIoClose(fd);
   T_flush_flag = 1;             /* the next WIN record's wall time includes this write */
   L_last_flush_us = sceKernelGetSystemTimeWide();
}

static void tlm_emit_win(unsigned frame, unsigned loop_iter, int loop_phase)
{
   unsigned char *r = tlm_new(1, frame);
   int b, i;
   if (r)
   {
      put16(r + 12, T_win.n);
      put16(r + 14, T_win.over);
      put32(r + 16, (unsigned)T_win.wall);
      put32(r + 20, (unsigned)T_win.work);
      put32(r + 24, T_win.wall_max);
      put32(r + 28, T_win.work_max);
      for (i = 0; i < 8; i++)
         put16(r + 32 + 2 * i, T_win.hist[i] > 0xFFFF ? 0xFFFF : T_win.hist[i]);
      put32(r + 48, flush_rom_total);
      put32(r + 52, &dma_smc_flushes ? dma_smc_flushes : 0);
      put16(r + 56, badjump_contained);
      put16(r + 58, &badjump_recoveries ? badjump_recoveries : 0);
      put16(r + 60, fe_evt_drops());
      b = batt_pct();
      r[62] = (unsigned char)(b < 0 ? 255 : b);
      r[63] = (unsigned char)loop_phase;
      T_flush_flag = 0;
   }
   if (++T_wins % 10 == 0 && (r = tlm_new(3, frame)) != NULL)
   {
      unsigned v[13];
      memset(v, 0, sizeof(v));
      if (&jit_code_stats)
      {
         v[0] = jit_code_stats[0]; v[1] = jit_code_stats[2]; v[2] = jit_code_stats[3]; v[3] = jit_code_stats[4];
         v[4] = jit_code_stats[5]; v[5] = jit_code_stats[6]; v[6] = jit_code_stats[7];
      }
      v[7] = gamepak_reopens;
      v[8] = gamepak_short_reads;
      v[9] = (unsigned)sceKernelTotalFreeMemSize() / 1024u;
      v[10] = (unsigned)sceKernelMaxFreeMemSize() / 1024u;
      v[11] = loop_iter;
      for (i = 0; i < 13; i++)
         put32(r + 12 + 4 * i, v[i]);
   }
   memset(&T_win, 0, sizeof(T_win));
}

int lab_frame(unsigned frame, unsigned wall_us, unsigned work_us, unsigned loop_iter, int loop_phase)
{
   static const unsigned edge[7] = { 16740, 20000, 25000, 33400, 50000, 75000, 100000 };
   unsigned i;
   uint64_t now;
   if (!L_active)
      return 0;
   L_last_frame = frame;
   T_win.n++;
   T_win.wall += wall_us;
   T_win.work += work_us;
   if (wall_us > T_win.wall_max) T_win.wall_max = wall_us;
   if (work_us > T_win.work_max) T_win.work_max = work_us;
   if (work_us > 16740) T_win.over++;
   for (i = 0; i < 7 && wall_us >= edge[i]; i++) {}
   T_win.hist[i]++;
   if (T_win.n >= (unsigned)L_tlm_every)
   {
      tlm_emit_win(frame, loop_iter, loop_phase);
      now = sceKernelGetSystemTimeWide();
      if (L_tlm_flush_s > 0 && now - L_last_flush_us >= (uint64_t)L_tlm_flush_s * 1000000ull)
         tlm_flush();
      if (L_wall_limit_s > 0 && now - L_t0_us >= (uint64_t)L_wall_limit_s * 1000000ull)
         return 1;
   }
   return 0;
}

/* ----------------------------------------------------------------- tasks */
static int task_ini_int(const char *key, int def)
{
   return (int)fe_ini_get_int(L_ini, key, def);
}

void lab_audit_keys(void)
{
   static const char *const keys[] = { "lab_v", "lab_task", "lab_build", "lab_fixture", "lab_files", "lab_state",
                                       "lab_sav", "lab_attempts", "lab_wall_limit_s", "lab_tlm_every",
                                       "lab_tlm_flush_s", "lab_window_after", "lab_window_s" };
   char tmp[256];
   unsigned i;
   if (!L_active)
      return;
   for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
      fe_ini_get(L_ini, keys[i], tmp, sizeof(tmp));
}

int lab_active(void)
{
   return L_active;
}

void lab_set_eboot_crc(unsigned crc)
{
   L_eboot_crc = crc;
}

/* sec. 5.2 lab_files: name:size[:crc] for every staged input; "rom" names the ROM. */
static int files_ok(const char *tid, const char *rom, char *why, int why_sz)
{
   char list[512], *s, *save = NULL, p[192];
   if (!fe_ini_get(L_ini, "lab_files", list, sizeof(list)))
      return 1;
   for (s = strtok_r(list, ",", &save); s; s = strtok_r(NULL, ",", &save))
   {
      char *c1 = strchr(s, ':');
      int want;
      if (!c1)
         continue;
      *c1 = 0;
      want = (int)strtol(c1 + 1, NULL, 10);
      if (!strcmp(s, "rom"))
         snprintf(p, sizeof(p), "%s/roms/%s", L_root, rom);
      else
         snprintf(p, sizeof(p), "%s/tasks/%s/%s", L_root, tid, s);
      if (fsize(p) != want)
      {
         snprintf(why, why_sz, "file_mismatch:%s", s);
         return 0;
      }
   }
   return 1;
}

static void task_infra(const char *tid, const char *bid, const char *why)
{
   journal("task %s infra %s", tid, why);
   write_result(tid, bid, "infra", 2, why, 0, 0, 0, 0, 0);
}

/* Pick the first queued task without a RESULT.TXT.  Returns 1 and fills L_tid. */
static int pick_task(void)
{
   static char q[4096];
   char p[160], *line, *save = NULL;
   path2(p, sizeof(p), L_root, "QUEUE.TXT");
   if (rd(p, q, sizeof(q)) <= 0)
      return 0;
   for (line = strtok_r(q, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save))
   {
      const char *tid;
      size_t i, n;
      if (line[0] != 't' || line[1] != ' ')
         continue;
      tid = line + 2;
      n = strlen(tid);
      if (!n || n > 40)
         continue;
      for (i = 0; i < n; i++)
         if (!((tid[i] >= 'a' && tid[i] <= 'z') || (tid[i] >= '0' && tid[i] <= '9') || tid[i] == '_' || tid[i] == '-'))
            break;
      if (i != n)
         continue;
      snprintf(p, sizeof(p), "%s/out/%s/RESULT.TXT", L_root, tid);
      if (exists(p))
         continue;
      snprintf(p, sizeof(p), "%s/tasks/%s/TASK.INI", L_root, tid);
      if (!exists(p))
         continue;
      snprintf(L_tid, sizeof(L_tid), "%s", tid);
      snprintf(L_ini, sizeof(L_ini), "%s", p);
      return 1;
   }
   return 0;
}

static void process_ack(void)
{
   char buf[4096], p[160], tok[64], *line, *save = NULL;
   path2(p, sizeof(p), L_root, "ACK.TXT");
   if (rd(p, buf, sizeof(buf)) <= 0)
      return;
   if (!kv(buf, "token", tok, sizeof(tok)) || strcmp(tok, L_b.last_token) || !tok[0])
      return;               /* not from the PC that served our last window */
   for (line = strtok_r(buf, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save))
   {
      char tid[48];
      int size;
      if (line[0] != 'a' || sscanf(line, "a %47s %d", tid, &size) != 2 || strchr(tid, '/') || strchr(tid, '.') == tid)
         continue;
      snprintf(p, sizeof(p), "%s/out/%s/RESULT.TXT", L_root, tid);
      if (fsize(p) == size)
      {
         snprintf(p, sizeof(p), "%s/out/%s", L_root, tid);
         rmtree(p, 0);
      }
   }
}

static void process_cmd(void)
{
   char buf[512], p[64], tok[64], cmd[8];
   unsigned seq;
   path2(p, sizeof(p), L_root, "CMD.TXT");
   if (rd(p, buf, sizeof(buf)) <= 0)
      return;
   kv(buf, "token", tok, sizeof(tok));
   kv(buf, "cmd", cmd, sizeof(cmd));
   seq = kvu(buf, "seq", 0);
   if (!tok[0] || strcmp(tok, L_b.last_token) || seq <= L_b.cmd_seq)
      return;               /* stale: written for another window (H3) */
   if (strcmp(cmd, "RUN") && strcmp(cmd, "HOLD") && strcmp(cmd, "EXIT"))
      return;
   L_b.cmd_seq = seq;
   snprintf(L_b.mode, sizeof(L_b.mode), "%s", cmd);
   L_b.next_window_s = kvu(buf, "next_window_s", 30);
   if (L_b.next_window_s < 20 || L_b.next_window_s > 600)
      L_b.next_window_s = 30;
   journal("cmd %s seq=%u next_window_s=%u", cmd, seq, L_b.next_window_s);
}

static void recover_current(void)
{
   char buf[256], p[64], tid[48], bid[16], hp[160];
   path2(p, sizeof(p), L_root, "sys/CURRENT.TXT");
   if (rd(p, buf, sizeof(buf)) <= 0)
      return;
   if (kv(buf, "task", tid, sizeof(tid)) && tid[0])
   {
      int hang;
      kv(buf, "build", bid, sizeof(bid));
      snprintf(hp, sizeof(hp), "%s/out/%s/HANG.TXT", L_root, tid);
      hang = exists(hp);
      journal("previous boot died in %s -> %s", tid, hang ? "hang" : "crash");
      /* telemetry flushed before the death stays in out/<tid>/TLM.BIN */
      write_result(tid, bid, hang ? "hang" : "crash", -1, hang ? "stall" : "no_exit_path", 0, 0, 0, 0, 0);
   }
   sceIoRemove(p);
}

int lab_boot(const char *dir_base, char *harness, size_t harness_sz, char *rom_dir, size_t rom_sz,
             char *log_dir, size_t log_sz, char *log_path, size_t log_path_sz)
{
   char p[192], rom[96], bid[16], why[64], cur[256];
   int n;
   const char *b = dir_base;
   /* sec. 6.1.1: only the launcher dir or a lab build dir makes this a lab launch */
   if (strncasecmp(b, "ms0:/", 5))
      return 0;
   if (!strcasecmp(b + 4, "/PSP/GAME/GBALAB"))
      L_self_bid[0] = 0;
   else if (!strncasecmp(b + 4, "/GBALAB/builds/", 15) && strlen(b + 19) == 12)
      snprintf(L_self_bid, sizeof(L_self_bid), "%s", b + 19);
   else
      return 0;
   snprintf(L_root, sizeof(L_root), "%.4s/GBALAB", b);
   snprintf(L_self, sizeof(L_self), "%s/EBOOT.PBP", b);
   L_model = (unsigned)kuKernelGetModel();
   path2(p, sizeof(p), L_root, "sys");
   sceIoMkdir(L_root, 0777);
   sceIoMkdir(p, 0777);
   path2(p, sizeof(p), L_root, "out");
   sceIoMkdir(p, 0777);

   path2(p, sizeof(p), L_root, "NOAUTO");
   boot_load();
   L_b.boot++;
   if (exists(p))
   {
      boot_save();
      journal("NOAUTO present: exit to the XMB");
      sceKernelExitGame();
   }
   {  /* sec. 6.1.3: a window advertised by an earlier boot is closed now (H3) */
      char wbuf[1024];
      path2(p, sizeof(p), L_root, "sys/WINDOW.TXT");
      if (rd(p, wbuf, sizeof(wbuf)) > 0)
      {
         char *s = strstr(wbuf, "state=open"), nbuf[1100];
         if (s)
         {
            int m = snprintf(nbuf, sizeof(nbuf), "%.*sstate=closed%s", (int)(s - wbuf), wbuf, s + 10);
            wr(p, nbuf, m < (int)sizeof(nbuf) ? m : (int)sizeof(nbuf) - 1, 0);
         }
      }
   }
   labid_ensure();
   process_cmd();
   process_ack();
   boot_save();
   if (!strcmp(L_b.mode, "EXIT"))
   {
      journal("CMD=EXIT: leaving the loop to the XMB");
      strcpy(L_b.mode, "RUN");             /* a manual relaunch by the owner means go */
      boot_save();
      sceKernelExitGame();
   }
   recover_current();
   if (!strcmp(L_b.mode, "HOLD") || !pick_task())
   {
      unsigned s = L_b.idle_s < 30 ? 30 : L_b.idle_s;
      L_b.idle_s = s * 2 > 300 ? 300 : s * 2;
      lab_window("idle", s);
   }
   L_b.idle_s = 30;
   /* the task's identity, from its own ini */
   if (fe_ini_get_int(L_ini, "lab_v", 0) > LAB_HARNESS_V)
   {
      task_infra(L_tid, "", "protocol_version");
      boot_save();
      lab_relaunch_and_die(L_self);
   }
   fe_ini_get(L_ini, "lab_build", bid, sizeof(bid));
   fe_ini_get(L_ini, "lab_fixture", L_fixture, sizeof(L_fixture));
   snprintf(L_bid, sizeof(L_bid), "%s", bid);
   snprintf(p, sizeof(p), "%s/builds/%s/EBOOT.PBP", L_root, bid);
   if (strlen(bid) != 12 || !exists(p))
   {
      task_infra(L_tid, bid, "build_missing");
      boot_save();
      lab_relaunch_and_die(L_self);
   }
   if (strcmp(bid, L_self_bid))
   {
      /* sec. 6.1.9: hop into the task's build; two hops without a start = a loop */
      if (++L_b.hop > 2)
      {
         L_b.hop = 0;
         task_infra(L_tid, bid, "hop_loop");
         boot_save();
         lab_relaunch_and_die(L_self);
      }
      boot_save();
      journal("hop to %s for %s", bid, L_tid);
      lab_relaunch_and_die(p);
   }
   fe_ini_get(L_ini, "rom", rom, sizeof(rom));
   if (!files_ok(L_tid, rom, why, sizeof(why)))
   {
      task_infra(L_tid, bid, why);
      boot_save();
      lab_relaunch_and_die(L_self);
   }
   L_b.hop = 0;
   boot_save();
   L_attempts = task_ini_int("lab_attempts", 1);
   L_wall_limit_s = task_ini_int("lab_wall_limit_s", 900);
   L_tlm_every = task_ini_int("lab_tlm_every", 300);
   if (L_tlm_every < 30 || L_tlm_every > 6000)
      L_tlm_every = 300;
   L_tlm_flush_s = task_ini_int("lab_tlm_flush_s", 60);
   L_window_after = task_ini_int("lab_window_after", 1);
   L_window_s = task_ini_int("lab_window_s", 30);
   if (L_window_s < 20 || L_window_s > 600)
      L_window_s = 30;
   {
      char st[64];
      if (fe_ini_get(L_ini, "lab_state", st, sizeof(st)) && st[0] && !strchr(st, '/'))
         snprintf(L_state_in, sizeof(L_state_in), "%s/tasks/%s/%s", L_root, L_tid, st);
      if (fe_ini_get(L_ini, "lab_sav", st, sizeof(st)) && st[0] && !strchr(st, '/'))
         snprintf(L_sav_in, sizeof(L_sav_in), "%s/tasks/%s/%s", L_root, L_tid, st);
   }
   /* sec. 6.1.10: start */
   snprintf(L_out, sizeof(L_out), "%s/out/%s", L_root, L_tid);
   rmtree(L_out, 0);                   /* a half-written earlier attempt */
   sceIoMkdir(L_out, 0777);
   snprintf(L_work, sizeof(L_work), "%s/work", L_out);
   sceIoMkdir(L_work, 0777);
   n = snprintf(cur, sizeof(cur), "v=1\ntask=%s\nbuild=%s\nboot=%u\nattempt=%u\n", L_tid, bid, L_b.boot, L_attempt);
   path2(p, sizeof(p), L_root, "sys/CURRENT.TXT");
   wr(p, cur, n, 0);
   journal("task %s start build=%s", L_tid, bid);
   snprintf(harness, harness_sz, "%s", L_ini);
   snprintf(rom_dir, rom_sz, "%s/roms", L_root);
   snprintf(log_dir, log_sz, "%s", L_out);
   snprintf(log_path, log_path_sz, "%s/frontend.log", L_out);
   L_t0_us = L_last_flush_us = sceKernelGetSystemTimeWide();
   L_batt_start = batt_pct();
   L_active = 1;
   lab_event(1, 0, L_tid);
   return 1;
}

void lab_sidecars(char *save_path, size_t save_sz, char *state_path, size_t state_sz)
{
   const char *s;
   char p[192];
   if (!L_active)
      return;
   s = strrchr(save_path, '/');
   snprintf(p, sizeof(p), "%s/%s", L_work, s ? s + 1 : save_path);
   snprintf(save_path, save_sz, "%s", p);
   if (L_sav_in[0] && !exists(save_path))
      copy_file(L_sav_in, save_path);
   s = strrchr(state_path, '/');
   snprintf(p, sizeof(p), "%s/%s", L_work, s ? s + 1 : state_path);
   snprintf(state_path, state_sz, "%s", p);
   if (L_state_in[0] && !exists(state_path))
      copy_file(L_state_in, state_path);
}

void lab_finish(int exit_code, const char *reason)
{
   char p[160], ini[2048];
   int n;
   uint32_t ini_crc;
   unsigned t_ms;
   const char *outcome;
   if (!L_active)
      return;
   t_ms = (unsigned)((sceKernelGetSystemTimeWide() - L_t0_us) / 1000ull);
   if (T_win.n)
      tlm_emit_win(L_last_frame, 0, 0);
   lab_event(4, (unsigned)exit_code, reason ? reason : "ok");
   if (T_dropped)
      lab_event(5, T_dropped, "ring_drop");
   tlm_flush();
   n = rd(L_ini, ini, sizeof(ini));
   ini_crc = n > 0 ? crc32_buf(0, (const unsigned char *)ini, n) : 0;
   outcome = exit_code == 0 ? "ok" : (reason && !strcmp(reason, "lab_wall_limit")) ? "timeout" : "exit_error";
   write_result(L_tid, L_bid, outcome, exit_code, reason, L_last_frame, t_ms, T_written, T_dropped, ini_crc);
   path2(p, sizeof(p), L_root, "sys/CURRENT.TXT");
   sceIoRemove(p);
   if (exit_code == 0)
   {  /* autoboot's relaunch budget: refilled only by a task that completed (sec. 10.3, provisional) */
      path2(p, sizeof(p), L_root, "sys/BUDGET.TXT");
      wr(p, "v=1\nleft=3\nby=harness\n", 21, 0);
   }
   journal("task %s %s exit=%d reason=%s t_ms=%u tlm=%u", L_tid, outcome, exit_code, reason ? reason : "ok",
           t_ms, T_written);
   L_active = 0;
   if (L_window_after)
   {
      unsigned s = L_window_s;
      if (L_b.next_window_s > s)
         s = L_b.next_window_s;
      lab_window("after_task", s);
   }
   boot_save();
   lab_relaunch_and_die(L_self);
}
#endif /* LAB_HARNESS */

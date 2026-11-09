#if !defined(GPSP_PLAYABLE) || defined(GPSP_KEEP_TELEMETRY) || \
    defined(GPSP_STALL_RECORDER)
/* ---- STALL WATCH (harness `stall_watch_s`; 0 = off, the default) ---------
 *
 * The battle-cycle freeze leaves no badjump.txt, no exit line and no
 * `me_rend off`, and a log that simply stops cannot tell apart
 *   (a) the whole CPU halted (e.g. an unhandled exception),
 *   (b) the main thread spinning -- inside translated code or a frontend
 *       loop -- which starves every thread below 0x20, the log writer
 *       included, so the log stops while the console still "runs", and
 *   (c) the main thread blocked in a kernel wait that never returns.
 * This thread outranks everything we own and sleeps a second at a time.  Only
 * when the main loop has not begun an iteration for `stall_watch_s` seconds
 * does it write one line to log/stall.txt (raw sceIo, open/write/close per
 * line: no stdio lock the stuck thread could hold, and the close is the FAT
 * commit -- see standby_note).  Reading the result:
 *   no file                    -> (a)
 *   main st=RUNNING/READY, run+ large, phase 2 -> (b) in the core/JIT
 *   phase 7                    -> (b)/(c) inside me_rend_frame
 *   main st=WAITING            -> (c); waitType/waitId say on what
 *   me_beat moving, me_done==me_cmd -> the ME is alive and idle */
static int      g_stall_s;
/* HEARTBEAT (harness `heartbeat_s`, GPSP_PERF_RIG builds).  The same 0x10
 * thread appends one line to log/heartbeat.txt every heartbeat_s seconds,
 * whether or not anything is stuck: raw sceIo, open/append/close, so it owes
 * nothing to the event ring or to stdio.  A rig judges liveness from THIS
 * (and RESULT.TXT's frames=), never from frontend.log growing -- the log
 * writer sits below the emulation thread and starves silently
 * (log-starvation-false-freeze).  After a hard freeze the last line says how
 * far the run got and whether the main loop was still turning. */
#ifdef GPSP_PERF_RIG
static int      g_hb_s;
static char     g_hb_id[112];
#endif
static volatile int g_stall_run;
static SceUID   g_stall_thid = -1;

static int stall_refer(SceUID thid, SceKernelThreadInfo *ti)
{
   memset(ti, 0, sizeof(*ti));
   ti->size = sizeof(*ti);
   return thid < 0 ? -1 : sceKernelReferThreadStatus(thid, ti);
}

static void stall_record(unsigned iter)
{
   SceKernelThreadInfo m0, m1, au, io;
   char line[512], path[176];
   unsigned cs = 0, ds = 0, is = 0, b0 = 0, b1 = 0;
   int n, fd, rm0, rm1, rau, rio;

   me_host_diag(&cs, &ds, &is, &b0);
   rm0 = stall_refer(g_emu_thid, &m0);
   sceKernelDelayThread(100000);         /* is anything still moving? */
   me_host_diag(NULL, NULL, NULL, &b1);
   rm1 = stall_refer(g_emu_thid, &m1);
   rau = stall_refer(audio_thid, &au);
   rio = stall_refer(g_io_thid, &io);
   n = snprintf(line, sizeof(line),
                "stall t=%u iter=%u phase=%d frame=%u rend=%d "
                "me_cmd=%u me_done=%u me_input=%u me_beat=%u->%u "
                "ring=%u pc=%08x lr=%08x "
                "main=%d/st%d/pri0x%02x/wt%d/wid%08x/run+%u "
                "audio=%d/st%d/wt%d io=%d/st%d/wt%d\n",
                (unsigned)sceKernelGetSystemTimeLow(), iter, g_loop_phase,
                (unsigned)fe_host_frame_count(), g_me_rend, cs, ds, is, b0, b1,
                ring_w - ring_r, reg[15], reg[14],
                rm1, m1.status, (unsigned)m1.currentPriority, m1.waitType,
                (unsigned)m1.waitId,
                (rm0 >= 0 && rm1 >= 0)
                   ? (unsigned)(m1.runClocks.low - m0.runClocks.low) : 0u,
                rau, au.status, au.waitType, rio, io.status, io.waitType);
   if (n <= 0)
      return;
   if (n >= (int)sizeof(line))
      n = (int)sizeof(line) - 1;
#ifdef GBA_LINK
   /* the machine pair's position, appended to the same line */
   if (n > 0 && line[n - 1] == '\n' && n < (int)sizeof(line) - 2)
   {
      int k;
      line[n - 1] = ' ';
      k = fe_gbadual_probe(line + n, sizeof(line) - (size_t)n - 1);
      if (k > 0)
         n += (k < (int)sizeof(line) - n - 1) ? k : (int)sizeof(line) - n - 2;
      line[n++] = '\n';
   }
#endif
   /* A main thread SPINNING at 0x20 starves the io thread (0x22), which may
    * be parked inside the FAT driver holding its lock -- and then our own
    * sceIoOpen below would block and read as "no file = CPU halted".  Drop
    * main below io for the write (this also lets the writer drain the event
    * ring into frontend.log), then put it back in case this was a false
    * alarm. */
   if (rm1 >= 0 && m1.currentPriority < 0x30)
      sceKernelChangeThreadPriority(g_emu_thid, 0x30);
   sceKernelDelayThread(300000);
   snprintf(path, sizeof(path), "%s/stall.txt", LOG_DIR);
   fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
   if (fd >= 0)
   {
      sceIoWrite(fd, line, (SceSize)n);
      sceIoClose(fd);
   }
   if (rm1 >= 0 && m1.currentPriority < 0x30)
      sceKernelChangeThreadPriority(g_emu_thid, m1.currentPriority);
}

#ifdef GPSP_PERF_RIG
static void heartbeat_write(unsigned secs)
{
   char line[384], path[176], pair[160] = "";
   int n, fd;
#ifdef GBA_LINK
   /* GBA link mirror: where the machine pair is (docs/GBA-LINK-MIRROR.md
    * section 13) -- a pair looping without frames shows sw= moving with f=
    * still; a core stuck inside run() shows the same run= on every line. */
   pair[0] = ' ';
   fe_gbadual_probe(pair + 1, sizeof(pair) - 1);
#endif
   n = snprintf(line, sizeof(line),
                "hb s=%u t_ms=%u iter=%u phase=%d frame=%u evt_drop=%u "
                "shash=%u %s%s\n",
                secs, (unsigned)(sceKernelGetSystemTimeWide() / 1000ull),
                g_loop_iter, g_loop_phase, (unsigned)fe_host_frame_count(),
                fe_evt_drops(), g_shash_lines, g_hb_id, pair);
   if (n <= 0)
      return;
   if (n >= (int)sizeof(line))
      n = (int)sizeof(line) - 1;
   snprintf(path, sizeof(path), "%s/heartbeat.txt", LOG_DIR);
   fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
   if (fd >= 0)
   {
      sceIoWrite(fd, line, (SceSize)n);
      sceIoClose(fd);
   }
}
#endif

#ifdef GPSP_PERF_RIG
/* STALL EXIT (harness `stall_exit_s`; 0 = off, the default.  GBA-LINK-
 * MIRROR.md 22).  A rig console that stops cannot be recovered by anyone but
 * the owner, so a run may ask not to be left spinning: when the main loop
 * has not begun an iteration for stall_exit_s seconds this thread
 *   1. writes the stall record (stall.txt: thread states, phase, ME beat,
 *      the pair) and a heartbeat line -- stall_record drops the main thread
 *      below the io thread for the write, which also lets the log drain;
 *   2. asks both link cores to stop at their next scheduler call
 *      (fe_gbadual_abort): a pair that is spinning inside the machines then
 *      fails E_OVERRUN and the bench ends through `gbadual_failed`, a
 *      session through `gblink_failed`;
 *   3. only with `stall_exit_hard = 1`, and only if the loop is still not
 *      turning 3 s later, leaves the process (sceKernelExitGame from this
 *      thread).  NOT YET RUN ON HARDWARE OR UNDER PSPLINK.
 * It cannot help when this thread itself no longer runs (a halted CPU). */
static int g_stall_exit_s, g_stall_exit_hard;
static void stall_exit(unsigned iter)
{
   stall_record(iter);
   if (g_hb_s > 0)
      heartbeat_write(100000u + (unsigned)g_stall_exit_s);
#ifdef GBA_LINK
   fe_gbadual_abort();
#endif
   sceKernelDelayThread(3000000); /* @lab */ LAB_HOOK(if (g_loop_iter == iter) lab_stalled(iter, g_loop_phase);)
   if (g_loop_iter != iter || !g_stall_exit_hard)
      return;
   if (g_hb_s > 0)
      heartbeat_write(200000u + (unsigned)g_stall_exit_s);
   FE_EXIT_GAME();
}
#endif

static int stall_thread(SceSize args, void *argp)
{
   unsigned last = g_loop_iter, still_s = 0, records = 0;
#ifdef GPSP_PERF_RIG
   unsigned secs = 0, x_last = g_loop_iter, x_still = 0, x_done = 0;
#endif
   (void)args; (void)argp;
   while (g_stall_run)
   {
      unsigned it;
      sceKernelDelayThread(1000000);
      if (!g_stall_run)
         break;
#ifdef GPSP_PERF_RIG
      secs++;
      if (g_hb_s > 0 && secs % (unsigned)g_hb_s == 0)
         heartbeat_write(secs);
      if (g_stall_exit_s > 0 && !x_done)
      {
         unsigned xi = g_loop_iter;
         if (xi != x_last)
         {
            x_last = xi;
            x_still = 0;
         }
         else if (++x_still >= (unsigned)g_stall_exit_s)
         {
            x_done = 1;
            stall_exit(xi);
         }
      }
      if (g_stall_s <= 0)
         continue;              /* heartbeat only: no stall records */
#endif
      it = g_loop_iter;
      if (it != last)
      {
         last = it;
         still_s = 0;
         continue;
      }
      if (++still_s < (unsigned)g_stall_s || records >= 8)
         continue;
      still_s = 0;              /* the next record needs another full window */
      records++;
      stall_record(it);
   }
   return 0;
}

static void stall_watch_start(void)
{
#ifdef GPSP_PERF_RIG
   if (g_stall_s <= 0 && g_hb_s <= 0 && g_stall_exit_s <= 0)
#else
   if (g_stall_s <= 0)
#endif
      return;
   g_stall_run  = 1;
   g_stall_thid = sceKernelCreateThread("gpsp_stall", stall_thread, 0x10,
                                        0x2000, THREAD_ATTR_USER, NULL);
   if (g_stall_thid < 0 || sceKernelStartThread(g_stall_thid, 0, NULL) < 0)
   {
      if (g_stall_thid >= 0)
         sceKernelDeleteThread(g_stall_thid);
      g_stall_thid = -1;
      g_stall_run  = 0;
      fe_evt("stall_watch off reason=thread_failed");
      return;
   }
   fe_evt("stall_watch on s=%d prio=0x10 file=stall.txt", g_stall_s);
}

static void stall_watch_stop(void)
{
   SceUInt tmo = 2000000;          /* the thread sleeps 1 s at a time */
   if (g_stall_thid < 0)
      return;
   g_stall_run = 0;
   if (sceKernelWaitThreadEnd(g_stall_thid, &tmo) < 0)
      sceKernelTerminateDeleteThread(g_stall_thid);
   else
      sceKernelDeleteThread(g_stall_thid);
   g_stall_thid = -1;
}
#else
#define stall_watch_start() do { } while (0)
#define stall_watch_stop()  do { } while (0)
#endif

/* ---- DRPROF_HW: the dynarec time-split sampler (docs/DYNAREC-PROFILE.md) --
 *
 * A thread above the emulator wakes every `drprof_us` microseconds (harness
 * ini, default 1000), asks the kernel whether the emulation thread was RUNNING
 * when it was preempted (it is then READY), and if so reads the phase word the
 * core publishes in reg[40] (drprof.h: translated code 0, update_gba 1,
 * lookup 2, flush 3, ...).  Samples taken while the emulator was WAITING are
 * counted but not attributed.  The main loop turns the histogram into
 *   EVT drprof f=<frame> n=<samples> run=<attributed> wait=<not running>
 *               ph=<16 counts>
 * every 300 frames (and at exit), so windows map onto the script's scenes by
 * frame.  Cost: two thread switches and one syscall per sample -- priced by
 * running the same fixture without DRPROF_HW (GBADHOC-DRPROF A vs S). */
#ifdef DRPROF_HW
static volatile int g_drs_run;
static SceUID g_drs_thid = -1;
static int g_drs_us = 1000;
static volatile unsigned g_drs_hist[16], g_drs_n, g_drs_att, g_drs_wait;
/* Link sessions (GBA-LINK-MIRROR.md 16): where the frontend is
 * (fe_linkprof.h FELP_*), and inside the machines which instance runs and
 * ITS phase word -- A's reg[40], B's gbab_reg[40]. */
#ifdef GBA_LINK
extern u32 gbab_reg[];
int fe_gbadual_running_inst(void);
#endif
static volatile unsigned g_lps_fe[FELP_N], g_lps_a[16], g_lps_b[16],
                         g_lps_pair, g_lps_l[FELP_LN];

static int drprof_sampler(SceSize args, void *argp)
{
   (void)args; (void)argp;
   while (g_drs_run)
   {
      SceKernelThreadRunStatus st;
      unsigned ph;
      sceKernelDelayThread((SceUInt)g_drs_us);
      ph = reg[40] & 15u;
      memset(&st, 0, sizeof(st));
      st.size = sizeof(st);
      g_drs_n++;
      if (g_emu_thid >= 0 &&
          sceKernelReferThreadRunStatus(g_emu_thid, &st) == 0 &&
          (st.status & PSP_THREAD_READY))
      {
         unsigned fe = fe_lp_phase & FELP_MASK;
         if (fe >= FELP_N + FELP_LN)
            fe = FELP_LOOP;
         g_drs_hist[ph]++;
         g_drs_att++;
#ifdef GBA_LINK
         {
            /* inside the pair (a session's advance or the one-console
             * bench's frame): the running instance's own phase word */
            int inst = fe_gbadual_running_inst();
            if (inst == 0)
               g_lps_a[ph]++;
            else if (inst == 1)
               g_lps_b[gbab_reg[40] & 15u]++;
            else if (fe == FELP_CORE)
               g_lps_pair++;
            if (inst >= 0 && (fe == FELP_LOOP || fe >= FELP_L0))
               fe = FELP_CORE;          /* the bench never sets FELP_CORE */
         }
#else
         if (fe == FELP_CORE)
            g_lps_a[ph]++;
#endif
         /* the loop's own regions: `fe=` counts them as loop, `lp=` splits
          * them (lp[15] = loop time outside every named region) */
         if (fe >= FELP_L0)
         {
            g_lps_l[fe - FELP_L0]++;
            fe = FELP_LOOP;
         }
         else if (fe == FELP_LOOP)
            g_lps_l[FELP_LN - 1]++;
         g_lps_fe[fe]++;
      }
      else
         g_drs_wait++;
   }
   return 0;
}

/* Every user thread's own CPU time over the window (runClocks, us), so the
 * sampler's phases -- which count a READY emulation thread, i.e. include
 * whatever higher-priority thread preempted it -- can be told apart from
 * other threads' work: `EVT thrcpu` one line per thread per window. */
#define DRS_THR_MAX 40
static SceUID   g_drt_id[DRS_THR_MAX];
static uint64_t g_drt_clk[DRS_THR_MAX];
static unsigned g_drt_n;
static uint64_t g_drt_t0;
static void drprof_threads(unsigned frame)
{
   SceUID ids[DRS_THR_MAX];
   int cnt = 0, i;
   uint64_t now = net_now_us();
   unsigned wall = g_drt_t0 ? (unsigned)(now - g_drt_t0) : 0;
   g_drt_t0 = now;
   if (sceKernelGetThreadmanIdList(SCE_KERNEL_TMID_Thread, ids, DRS_THR_MAX,
                                   &cnt) < 0)
      return;
   if (cnt > DRS_THR_MAX)
      cnt = DRS_THR_MAX;
   for (i = 0; i < cnt; i++)
   {
      SceKernelThreadInfo ti;
      uint64_t clk;
      unsigned j, us = 0;
      memset(&ti, 0, sizeof(ti));
      ti.size = sizeof(ti);
      if (sceKernelReferThreadStatus(ids[i], &ti) < 0)
         continue;
      clk = ((uint64_t)ti.runClocks.hi << 32) | ti.runClocks.low;
      for (j = 0; j < g_drt_n; j++)
         if (g_drt_id[j] == ids[i])
            break;
      if (j < g_drt_n)
         us = (unsigned)(clk - g_drt_clk[j]);
      else if (g_drt_n < DRS_THR_MAX)
      {
         j = g_drt_n++;
         g_drt_id[j] = ids[i];
      }
      else
         continue;
      g_drt_clk[j] = clk;
      if (wall)
         fe_evt("thrcpu f=%u wall=%u name=%.16s prio=0x%02X us=%u", frame,
                wall, ti.name, (unsigned)ti.currentPriority, us);
   }
}

static void drprof_window(unsigned frame)
{
   unsigned h[16], i, n = g_drs_n, a = g_drs_att, w = g_drs_wait;
   for (i = 0; i < 16; i++)
   {
      h[i] = g_drs_hist[i];
      g_drs_hist[i] = 0;
   }
   g_drs_n = g_drs_att = g_drs_wait = 0;
   fe_evt("drprof f=%u n=%u run=%u wait=%u ph=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,"
          "%u,%u,%u,%u,%u,%u", frame, n, a, w, h[0], h[1], h[2], h[3], h[4],
          h[5], h[6], h[7], h[8], h[9], h[10], h[11], h[12], h[13], h[14], h[15]);
   {
      /* EVT linkprof: fe = loop,core,hash,crc,net,sess,-,- ; a / b = the
       * instances' drprof phases while inside the machines; pair = inside
       * the machines but between the instances' runs (the pair's loop). */
      unsigned fe[FELP_N], pa[16], pb[16], pr = g_lps_pair;
      char sa[96], sb[96];
      for (i = 0; i < FELP_N; i++)
      {
         fe[i] = g_lps_fe[i];
         g_lps_fe[i] = 0;
      }
      for (i = 0; i < 16; i++)
      {
         pa[i] = g_lps_a[i];
         pb[i] = g_lps_b[i];
         g_lps_a[i] = g_lps_b[i] = 0;
      }
      g_lps_pair = 0;
      snprintf(sa, sizeof(sa), "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,"
               "%u", pa[0], pa[1], pa[2], pa[3], pa[4], pa[5], pa[6], pa[7],
               pa[8], pa[9], pa[10], pa[11], pa[12], pa[13], pa[14], pa[15]);
      snprintf(sb, sizeof(sb), "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,"
               "%u", pb[0], pb[1], pb[2], pb[3], pb[4], pb[5], pb[6], pb[7],
               pb[8], pb[9], pb[10], pb[11], pb[12], pb[13], pb[14], pb[15]);
      fe_evt("linkprof f=%u n=%u run=%u wait=%u fe=%u,%u,%u,%u,%u,%u,%u,%u "
             "a=%s b=%s pair=%u", frame, n, a, w, fe[0], fe[1], fe[2], fe[3],
             fe[4], fe[5], fe[6], fe[7], sa, sb, pr);
      {
         /* EVT looprof: the loop's own regions (FELP_L_*, section 17) */
         unsigned l[FELP_LN];
         for (i = 0; i < FELP_LN; i++)
         {
            l[i] = g_lps_l[i];
            g_lps_l[i] = 0;
         }
         fe_evt("looprof f=%u lp=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,"
                "%u,%u names=top,netf,gbls,misc,pres0,ap,run,post,shash,acct,"
                "osd,retire,pres,swap,rign,other", frame, l[0], l[1], l[2],
                l[3], l[4], l[5], l[6], l[7], l[8], l[9], l[10], l[11], l[12],
                l[13], l[14], l[15]);
      }
      drprof_threads(frame);
   }
}

static void drprof_start(void)
{
   g_drs_us = (int)fe_ini_get_int(HARNESS_INI, "drprof_us", 1000);
   if (g_drs_us <= 0)
   {
      fe_evt("drprof off reason=drprof_us");
      return;
   }
   if (g_drs_us < 200)
      g_drs_us = 200;
   g_drs_run = 1;
   g_drs_thid = sceKernelCreateThread("gpsp_drprof", drprof_sampler, 0x0F,
                                      0x1000, THREAD_ATTR_USER, NULL);
   if (g_drs_thid < 0 || sceKernelStartThread(g_drs_thid, 0, NULL) < 0)
   {
      if (g_drs_thid >= 0)
         sceKernelDeleteThread(g_drs_thid);
      g_drs_thid = -1;
      g_drs_run = 0;
      fe_evt("drprof off reason=thread_failed");
      return;
   }
   fe_evt("drprof on us=%d prio=0x0F phases=jit,upd,disp,flush,cmisc,-,xlat,"
          "memc,sound,dma,serial,video,irq,retro,-,outside", g_drs_us);
}

static void drprof_stop(unsigned frame)
{
   SceUInt tmo = 200000;
   if (g_drs_thid < 0)
      return;
   g_drs_run = 0;
   if (sceKernelWaitThreadEnd(g_drs_thid, &tmo) < 0)
      sceKernelTerminateDeleteThread(g_drs_thid);
   else
      sceKernelDeleteThread(g_drs_thid);
   g_drs_thid = -1;
   drprof_window(frame);
}
#define DRPROF_PHASE(p) (reg[40] = (p))
#else
#define drprof_start()       do { } while (0)
#define drprof_stop(f)       do { (void)(f); } while (0)
#define drprof_window(f)     do { (void)(f); } while (0)
#define DRPROF_PHASE(p)      do { } while (0)
#endif

#ifdef GPSP_PERF_RIG
/* HARNESS ONLY: the SMC gate table after every scripted state reload.  The
 * ranked-gate learner keeps its counters across savestate loads by design
 * (only a ROM load clears them), so a battle replayed N times keeps adding
 * evidence and a gate can first appear at a FIXED cycle -- which is what a
 * freeze at "cycle ~14 on both consoles" looks like from outside.  One line
 * per reload says when the table changed; `smc_gates` at exit only ever
 * showed the end state. */
static void smc_gates_reload_evt(void)
{
   extern unsigned int translation_gate_targets;
   extern unsigned int translation_gate_target_pc[];
   char gates[16 * 10];
   unsigned gn = translation_gate_targets, gi;
   int gp = 0;
   if (gn > 16)
      gn = 16;
   for (gi = 0; gi < gn; gi++)
      gp += snprintf(gates + gp, sizeof(gates) - gp, "%s%08x",
                     gi ? "," : "", translation_gate_target_pc[gi]);
   if (!gp)
      snprintf(gates, sizeof(gates), "-");
   fe_evt("smc_gates_reload f=%u n=%u pcs=%s", fe_host_frame_count(),
          translation_gate_targets, gates);
}
#endif

/* ---- core_health: the soak line (round 2, agreed with FRONTEND/DYNAREC) --
 *
 *   EVT core_health at=<hb|load> f=<frame> reopens=<n> short_reads=<n>
 *       badjump=<n> rom_flush=<n> [dma_flush=<n>]
 *       [xlat_depth=<n> stack_used=<B> sp_min=<a>]
 *
 * Every 600 frames (fe_host's heartbeat cadence; a separate line so the
 * parsed `heartbeat` format stays fixed) and right after every state load.
 * All counters are monotonic since boot.  The xlat fields exist only when the
 * core was built with DYNAREC's XLAT_DEPTH_PROBE (weak symbols, so every
 * flavour links); they are per-window and reset after each line, so the line
 * AFTER an `at=load` line prices the post-reload retranslation.  sp is
 * sampled one frame above the deepest translate_block, so stack_used reads
 * ~400 B LOW (DYNAREC). */
#if !defined(GPSP_PLAYABLE) || defined(GPSP_KEEP_TELEMETRY)
extern u32 gamepak_reopens, gamepak_short_reads, badjump_contained;
extern u32 xlat_depth_max __attribute__((weak));
extern u32 xlat_sp_min __attribute__((weak));
void xlat_depth_probe_reset(void) __attribute__((weak));
/* DMA_SMC_FLUSH (DYNAREC #7): HBlank/VBlank DMAs that hit translated code and
 * forced a full RAM flush.  Monotonic; ~1/frame on Unbound/H&S = a storm. */
extern u32 dma_smc_flushes __attribute__((weak));
/* JIT_CODE_DISCIPLINE (docs/JIT-CODE-DISCIPLINE.md): jit_code_stats_type is
 * ten u32s -- publishes, spans, lines, whole_i, early, then the ownership
 * changes by reason (ROM flush, RAM flush, rebuild, init, tier).  Weak, read
 * as words so the frontend needs none of the core's headers. */
extern u32 jit_code_stats[10] __attribute__((weak));
/* badjump_recoveries (BADJUMP_SAFE, declared above): unmappable guest pcs
 * caught and turned into a GBA reset.  A run whose script reloads the state
 * (reload_soak.txt) recovers from such a derail at the next load, so its last
 * frame cannot show one; this can. */

static void core_health_evt(const char *at)
{
   char xl[80] = "", dm[48] = "", jc[96] = "", fb[192] = "";
   if (&smc_fallback_stats)
   {
      unsigned i, n = (unsigned)snprintf(fb, sizeof(fb), " smcfb=");
      for (i = 0; i < 15 && n < sizeof(fb); i++)
         n += (unsigned)snprintf(fb + n, sizeof(fb) - n, i ? "/%u" : "%u",
                                 (unsigned)smc_fallback_stats[i]);
   }
   if (&dma_smc_flushes)
      snprintf(dm, sizeof(dm), " dma_flush=%u", (unsigned)dma_smc_flushes);
   if (&badjump_recoveries)
      snprintf(dm + strlen(dm), sizeof(dm) - strlen(dm), " bjrec=%u",
               (unsigned)badjump_recoveries);
   if (&jit_code_stats)
      snprintf(jc, sizeof(jc), " jitc=%u/%u/%u/%u own=%u/%u/%u",
               (unsigned)jit_code_stats[0], (unsigned)jit_code_stats[2],
               (unsigned)jit_code_stats[3], (unsigned)jit_code_stats[4],
               (unsigned)jit_code_stats[5], (unsigned)jit_code_stats[6],
               (unsigned)jit_code_stats[7]);
   if (&xlat_depth_max && &xlat_sp_min)
   {
      static unsigned stack_top;
      unsigned sp_min = xlat_sp_min, used = 0;
      if (!stack_top && g_emu_thid >= 0)
      {
         SceKernelThreadInfo ti;
         memset(&ti, 0, sizeof(ti));
         ti.size = sizeof(ti);
         if (sceKernelReferThreadStatus(g_emu_thid, &ti) >= 0)
            stack_top = (unsigned)(uintptr_t)ti.stack + (unsigned)ti.stackSize;
      }
      if (sp_min && stack_top > sp_min)
         used = stack_top - sp_min;
      snprintf(xl, sizeof(xl), " xlat_depth=%u stack_used=%u sp_min=%08x",
               (unsigned)xlat_depth_max, used, sp_min);
      if (xlat_depth_probe_reset)
         xlat_depth_probe_reset();
   }
   fe_evt("core_health at=%s f=%u reopens=%u short_reads=%u badjump=%u "
          "rom_flush=%u%s%s%s%s", at, fe_host_frame_count(),
          (unsigned)gamepak_reopens, (unsigned)gamepak_short_reads,
          (unsigned)badjump_contained, (unsigned)flush_rom_total, dm, xl, jc, fb);
#ifdef ROM_HASH_CENSUS
   /* cpu_threaded.c: the ROM hash's chain depth (an arm for long LARGE-tier
    * sessions; critique-7 SF4).  hit_hops_x100 = mean hops for a hit. */
   /* The walk is not free (critique-8 SF1: a full LARGE cache is ~44 K
    * dependent loads, estimated 8-13 ms), so it runs on every 10th
    * heartbeat (counted across reloads) and at every other call, and times itself. */
   {
      extern void rom_hash_census(u32 out[10]);
      static unsigned hb_calls;
      int is_hb = at[0] == 'h' && at[1] == 'b' && !at[2];
      if (!is_hb || (hb_calls++ % 10u) == 0)
      {
         u32 hc[10], t0, walk_us;
         t0 = sceKernelGetSystemTimeLow();
         rom_hash_census(hc);
         walk_us = sceKernelGetSystemTimeLow() - t0;
         fe_evt("rom_hash at=%s entries=%u buckets=%u/%u max_chain=%u "
                "hit_hops_x100=%u rom_used=%u chains=%u/%u/%u/%u walk_us=%u",
                at, (unsigned)hc[0], (unsigned)hc[1], (unsigned)hc[5],
                (unsigned)hc[2],
                hc[0] ? (unsigned)((unsigned long long)hc[3] * 100u / hc[0]) : 0u,
                (unsigned)hc[4], (unsigned)hc[6], (unsigned)hc[7],
                (unsigned)hc[8], (unsigned)hc[9], (unsigned)walk_us);
      }
   }
#endif
#ifdef JIT_COST_KEYS
   /* JIT_COST cores append kcalls and lineops (words 11, 12) after big_i
    * (word 10): the cache work publishes handed out, cumulative. */
   if (&jit_code_stats)
   {
      /* the declaration above is ten words; launder the pointer so the
       * compiler does not hold the longer JIT_COST struct to that bound */
      const volatile u32 *js = (const volatile u32 *)jit_code_stats;
      __asm__("" : "+r"(js));
      fe_evt("jit_cost_stats at=%s publishes=%u lines=%u whole_i=%u big_i=%u "
             "kcalls=%u lineops=%u", at, (unsigned)js[0], (unsigned)js[2],
             (unsigned)js[3], (unsigned)js[10], (unsigned)js[11],
             (unsigned)js[12]);
   }
#endif
}
#else
#define core_health_evt(at) do { } while (0)
#endif

/* STATE LOADS WAIT FOR THE MEDIA ENGINE (FRONTEND lead #15).
 *
 * Proof first, from the code: the ME never WRITES guest memory -- it writes
 * only its PRX data, the stage buffer and the mailbox -- and it reads guest
 * VRAM/OAM/palette only in phase 1 of a render, which me_rend_frame waits out
 * (input_seq) before emulation resumes; shadows, when on, are frontend
 * copies.  So retro_unserialize cannot be corrupted by the ME, and a render
 * in flight only reads its own PRX copies and a capture buffer the load does
 * not touch.  The one exception is an `input_wedge` teardown, which gives up
 * while the ME may still be reading live VRAM; its output is discarded.
 *
 * The wait below therefore buys determinism, not safety: a load never
 * overlaps ME activity of any kind, whatever path got us here.  Bounded by
 * the same 50 ms ceiling as the input handshake, costs at most one render
 * per load, and a timeout only logs.  Saves need nothing: retro_serialize
 * only reads guest memory, and the thumbnail reads the READY stage, which the
 * ME is not writing. */
static void me_rend_quiesce(const char *why)
{
   unsigned t0;
   FE_EVT_ONLY(why);
   if (!me_host_up() || me_host_idle())
      return;
   t0 = (unsigned)sceKernelGetSystemTimeLow();
   while (!me_host_idle() &&
          ((unsigned)sceKernelGetSystemTimeLow() - t0) < MER_INPUT_US)
      ;
   if (!me_host_idle())
      fe_evt("me_rend quiesce_timeout why=%s", why);
}

/* The single state-load entry point for this frontend: quiesce, load, log. */
static int psp_state_load_io(const char *path, const char *why)
{
   int rc;
   me_rend_quiesce(why);
   /* A loaded state starts with no turbo button firing: a Toggle is off
    * until it is pressed again, a Hold still down starts a fresh train. */
   ctl_turbo_stop(&g_turbo);
   rc = fe_host_state_load(path);
   core_health_evt("load"); /* @lab */ LAB_HOOK(lab_event(LAB_EVT_STATE_LOAD, (unsigned)rc, why);)
   return rc;
}

/* SOFT RESET ("Reset game", the in-game menu).  Everything a state load
 * does around the core -- land a rewind, quiesce the Media Engine renderer,
 * stop the turbo trains, hold the power switch -- then fe_host_reset (the
 * battery to the stick, then the fresh-boot reset; fe_host.h).  Refused in
 * any link or wireless session.  Same rc as fe_host_reset. */
static int psp_game_reset(void)
{
   int rc;
   if (link_session_any())
   {
#ifdef GBA_LINK
      fe_evt("game_reset REFUSED reason=session net=%d mg=%d gblink=%d "
             "pair=%d", g_net_up, g_mg_on, fe_host_gblink_active(),
             fe_gbadual_active());
#else
      fe_evt("game_reset REFUSED reason=session net=%d mg=%d gblink=%d",
             g_net_up, g_mg_on, fe_host_gblink_active());
#endif
      return -2;
   }
   pwr_hold("reset");
#ifdef REWIND
   (void)psp_rewind_land("reset", 0);
#endif
   me_rend_quiesce("reset");
   ctl_turbo_stop(&g_turbo);
   rc = fe_host_reset();
   if (rc == 0 && g_pcfg.console != FE_CONSOLE_GBA)
      g_last_pix = NULL;    /* the GB machine (and its frame buffer) is new */
   core_health_evt("reset");
   pwr_release();
   return rc;
}

/* pwr_hold: see power_cb -- this runs outside the loop's park point. */
static int psp_state_load(const char *path, const char *why)
{
   int rc;
   pwr_hold("state_load");
#ifdef REWIND
   (void)psp_rewind_land("state_load", 0);   /* (a failed load goes on here) */
#endif
   rc = psp_state_load_io(path, why);
   pwr_release();
   return rc;
}

/* ---- RIG IDENTITY AND HONESTY (docs/RIG-DOUBLE-BATTLE.md §2) ------------
 *
 * Everything here exists so a hardware run cannot be misread:
 *   rig_preserve_log   a relaunch used to truncate frontend.log; an
 *                      uncollected log is now copied to frontend.prev.log
 *   rig_boot_identity  psp_model (role verified by content, not drive letter),
 *                      run_id / arm (pairing and A/B identity), eboot_crc
 *   rig_rom_identity   which ROM image actually ran
 *   rig_cfg_audit_*    every harness/config key echoed; a key nothing reads
 *                      (a typo) is reported as cfg_unknown
 *   rig_fail_probe     the game state at the moment a script gave up
 *   rig_romload_evt    ROM page-ins from the stick per window (the PSP-1000)
 * Telemetry builds only; a release build compiles none of it. */
#if !defined(GPSP_PLAYABLE) || defined(GPSP_KEEP_TELEMETRY)
static int g_log_input = 1;          /* harness `log_input`, 0 = no input lines */
static char g_fail_probe[256];       /* harness `fail_probe`, read at boot */
/* harness `watch_ram` (up to 4 x addr:size): every CHANGE of a watched value
 * is logged with the frame it was first seen -- the link-room start-skew
 * metric (who moves first, and how long a console's own step takes to come
 * back through the link).  Capped so a busy address cannot flood the log. */
static uint32_t g_watch_addr[4], g_watch_val[4];
static uint8_t  g_watch_sz[4], g_watch_seen[4];
static int      g_watch_n, g_watch_left = 400;

/* FREEZE RIG: set at boot when log/frontend.log was still on the stick, i.e.
 * the previous run never reached the handoff where the PC collects (and
 * removes) it -- a hard freeze or a power loss.  See freeze_rig_recover
 * (ported from claude/freeze-rig 2d5b84d for the GBA link mirror's hang,
 * docs/GBA-LINK-MIRROR.md section 13). */
static int g_rig_prev_uncollected;

static void rig_preserve_log(void)
{
   char prev[200];
   static char buf[4096];
   SceUID in, out;
   int n;

   /* The per-frame oracle is truncated by the next run too, and after a
    * freeze it is the evidence of WHERE the guest diverged.  Keep one
    * generation (the rig collects and removes it after every clean run, so a
    * shash.txt still here at boot is an uncollected one). */
   {
      char cur[200], old[200];
      snprintf(cur, sizeof(cur), "%s/shash.txt", LOG_DIR);
      snprintf(old, sizeof(old), "%s/shash.prev.txt", LOG_DIR);
      if (file_exists(cur))
      {
         sceIoRemove(old);
         sceIoRename(cur, old);
      }
   }
   in = sceIoOpen(LOG_PATH, PSP_O_RDONLY, 0777);
   if (in < 0)
      return;
   g_rig_prev_uncollected = 1;
   snprintf(prev, sizeof(prev), "%s/frontend.prev.log", LOG_DIR);
   out = sceIoOpen(prev, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
   if (out >= 0)
   {
      while ((n = sceIoRead(in, buf, sizeof(buf))) > 0)
         sceIoWrite(out, buf, n);
      sceIoClose(out);
   }
   sceIoClose(in);
}

static const char *rig_model_name(int m)
{
   switch (m)
   {
   case 0:  return "PSP-1000";
   case 1:  return "PSP-2000";
   case 2: case 3: case 5: case 6: case 8: return "PSP-3000";
   case 4:  return "PSP-Go";
   case 10: return "PSP-E1000";
   default: return "unknown";
   }
}

static void rig_boot_identity(int have_harness)
{
   char rid[48] = "", arm[16] = "";
   int m = kuKernelGetModel();
   FE_EVT_ONLY(m);
   fe_evt("psp_model code=%d name=%s", m, rig_model_name(m));
   if (!have_harness)
      return;
   fe_ini_get(HARNESS_INI, "run_id", rid, sizeof(rid));
   fe_ini_get(HARNESS_INI, "arm", arm, sizeof(arm));
   fe_evt("rig run_id=%s arm=%s", rid[0] ? rid : "none", arm[0] ? arm : "none");
   g_log_input = (int)fe_ini_get_int(HARNESS_INI, "log_input", 1) ? 1 : 0;
   /* Read NOW, not at failure time: the key audit runs before the main loop,
    * and a key first read after it would be reported as unknown. */
   fe_ini_get(HARNESS_INI, "fail_probe", g_fail_probe, sizeof(g_fail_probe));
   {
      char wl[128], *p, *save = NULL;
      if (fe_ini_get(HARNESS_INI, "watch_ram", wl, sizeof(wl)))
         for (p = strtok_r(wl, ",", &save); p && g_watch_n < 4;
              p = strtok_r(NULL, ",", &save))
         {
            char *c = strchr(p, ':');
            unsigned sz = c ? (unsigned)strtoul(c + 1, NULL, 0) : 2;
            g_watch_addr[g_watch_n] = (uint32_t)strtoul(p, NULL, 0);
            g_watch_sz[g_watch_n] = (uint8_t)((sz == 1 || sz == 4) ? sz : 2);
            g_watch_n++;
         }
      fe_evt("watch_ram n=%d", g_watch_n);
   }
   fe_evt("log_input n=%d", g_log_input);
}

static void rig_rom_identity(const char *path)
{
   uint8_t h[0xC0];
   SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
   SceOff size;
   if (fd < 0)
   {
      fe_evt("rom_id err=open");
      return;
   }
   size = sceIoLseek(fd, 0, PSP_SEEK_END);
   sceIoLseek(fd, 0, PSP_SEEK_SET);
   if (sceIoRead(fd, h, sizeof(h)) != (int)sizeof(h))
   {
      sceIoClose(fd);
      fe_evt("rom_id err=short");
      return;
   }
   sceIoClose(fd);
   fe_evt("rom_id code=%.4s rev=%02x size=%u hdr_crc=%08x",
          (const char *)h + 0xAC, h[0xBC], (unsigned)size,
          (unsigned)fe_crc32(0, h, sizeof(h)));
}

static void rig_cfg_line(void *user, const char *key, const char *raw,
                         int asked, long ival, int is_int)
{
   (void)ival; (void)is_int;
   if (asked == 2)
      fe_evt("cfg_duplicate src=%s key=%s raw=%s ignored=1 (first line wins)",
             (const char *)user, key, raw);
   else if (asked)
      fe_evt("cfg src=%s key=%s raw=%s", (const char *)user, key, raw);
   else
      fe_evt("cfg_unknown src=%s key=%s raw=%s", (const char *)user, key, raw);
}

static void rig_cfg_audit_report(void)
{
   int n = fe_ini_audit_report(HARNESS_INI, rig_cfg_line, (void *)"harness");
   int c = fe_ini_audit_report(CONFIG_INI, rig_cfg_line, (void *)"config");
   FE_EVT_ONLY(n); FE_EVT_ONLY(c);
   fe_evt("cfg_audit harness_keys=%d config_keys=%d", n, c);
}

/* `fail_probe = 0x030030F4:4,0x02023E8A:1,...` (up to 8): read and print those
 * words the moment a script fails, so the failure names the game state. */
static void rig_fail_probe(void)
{
   char spec[256], out[320], *p, *save = NULL;
   int o = 0, k = 0;
   if (!g_fail_probe[0])
      return;
   strncpy(spec, g_fail_probe, sizeof(spec) - 1);
   spec[sizeof(spec) - 1] = '\0';
   for (p = strtok_r(spec, ",", &save); p && k < 8;
        p = strtok_r(NULL, ",", &save), k++)
   {
      char *colon = strchr(p, ':');
      uint32_t addr = (uint32_t)strtoul(p, NULL, 0), v = 0;
      unsigned sz = colon ? (unsigned)strtoul(colon + 1, NULL, 0) : 4;
      uint8_t b[4] = { 0, 0, 0, 0 };
      if (sz != 1 && sz != 2 && sz != 4)
         sz = 4;
      if (fe_host_mem_read(addr, b, sz) != 0)
         o += snprintf(out + o, sizeof(out) - o, " %08x=ERR", (unsigned)addr);
      else
      {
         v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
             ((uint32_t)b[3] << 24);
         o += snprintf(out + o, sizeof(out) - o, " %08x=%x", (unsigned)addr,
                       (unsigned)v);
      }
      if (o >= (int)sizeof(out) - 24)
         break;
   }
   out[o] = '\0';
   fe_evt("fail_probe%s", out);
}

static void rig_watch_evt(unsigned frames)
{
   int i;
   for (i = 0; i < g_watch_n && g_watch_left > 0; i++)
   {
      uint8_t b[4] = { 0, 0, 0, 0 };
      uint32_t v;
      if (fe_host_mem_read(g_watch_addr[i], b, g_watch_sz[i]) != 0)
         continue;
      v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
          ((uint32_t)b[3] << 24);
      if (g_watch_seen[i] && v == g_watch_val[i])
         continue;
      /* t_ms is the clock the autopilot's ap_mark lines carry, so a watch
       * change lines up with a script step on the same console. */
      fe_evt("watch a=%08x v=%x f=%u t_ms=%u", (unsigned)g_watch_addr[i],
             (unsigned)v, frames, (unsigned)(fe_evt_now_us() / 1000ull));
      g_watch_val[i] = v;
      g_watch_seen[i] = 1;
      g_watch_left--;
   }
}

static void rig_romload_evt(unsigned frames)
{
   static unsigned last_pg, last_re, last_sh;
   extern u32 gamepak_reopens, gamepak_short_reads;
   if (frames % 600u || !gamepak_must_swap())
      return;
   fe_evt("romload win=600 pg=%u reopen=%u short=%u",
          (unsigned)(gamepak_page_loads - last_pg),
          (unsigned)(gamepak_reopens - last_re),
          (unsigned)(gamepak_short_reads - last_sh));
   last_pg = gamepak_page_loads;
   last_re = gamepak_reopens;
   last_sh = gamepak_short_reads;
}
#else
#define g_log_input 0
#endif

/* EVT ff_win (docs/FF-PRESETS.md): one line per 10 s of wall time, so a
 * fast-forward preset can be scored as emulated fps beside drawn fps (new
 * images presented, g_ffw_drawn) and the mean retro_run cost.  `mode` is
 * what the loop is doing over the window's LAST frame: off, 3x, unl, smooth,
 * or harness (the `ff` key / autopilot `ff on`, which is not a user preset).
 * `gated` counts frames the draw gate decided not to draw, `resync` the draws
 * that found the gate a whole period behind (boundaries given up rather than
 * repaid as a burst: the gap between drawn fps and 30/60).  Telemetry only. */
#if !defined(GPSP_PLAYABLE) || defined(GPSP_KEEP_TELEMETRY)
#define FF_WIN_US 10000000u
static void ff_win_note(void)
{
   static uint64_t t0;
   static unsigned f0, d0, g0, c0, r0;
   static uint32_t u0;
   uint64_t now = net_now_us();
   unsigned calls = 0, f, d, g, n;
   uint32_t tot = 0, mx = 0;
   unsigned dt, emu_x100, drawn_x100, core_us;
   const char *mode;

   fe_host_core_prof(&calls, &tot, &mx);
   f = fe_host_frame_count();
   if (!t0)
   {
      t0 = now; f0 = f; d0 = g_ffw_drawn; g0 = g_ffw_gated;
      c0 = calls; u0 = tot; r0 = g_ff_gate_st.resyncs;
      return;
   }
   if (now - t0 < FF_WIN_US)
      return;
   dt = (unsigned)(now - t0);
   d = g_ffw_drawn - d0;
   g = g_ffw_gated - g0;
   n = calls - c0;
   emu_x100   = (unsigned)((uint64_t)(f - f0) * 100000000ull / dt);
   drawn_x100 = (unsigned)((uint64_t)d * 100000000ull / dt);
   core_us    = n ? (unsigned)((tot - u0) / n) : 0u;
   if (!g_ff_uncapped && !g_ff_mult)
      mode = "off";
   else if (g_ff_mult)
      mode = "3x";
   else if (!g_ff_user || g_pcfg.bench_mode)
      mode = "harness";
   else
      mode = pcfg_ff_mode() == PCFG_FF_SMOOTH ? "smooth" : "unl";
   fe_evt("ff_win mode=%s me=%d emu=%u.%02u drawn=%u.%02u core_us=%u "
          "frames=%u drawn_n=%u gated=%u resync=%u win_ms=%u",
          mode, g_me_rend, emu_x100 / 100, emu_x100 % 100,
          drawn_x100 / 100, drawn_x100 % 100, core_us, f - f0, d, g,
          g_ff_gate_st.resyncs - r0, dt / 1000u);
   t0 = now; f0 = f; d0 = g_ffw_drawn; g0 = g_ffw_gated;
   c0 = calls; u0 = tot; r0 = g_ff_gate_st.resyncs;
}
#else
#define ff_win_note()               do { } while (0)
#endif

/* ---- boot timeline (docs/UI-OVERLAY.md §12.2.1) ---------------------------
 * Microseconds since main() entry, and how many INI file opens (fe_ini_get
 * without a bound document, plus document loads) had run and how many found
 * no file, at each milestone up to the browser; doc_hits = the lookups
 * answered from CONFIG.INI's in-memory copy instead.  One EVT line in telemetry builds; nothing in a release. */
#if !defined(GPSP_PLAYABLE) || defined(GPSP_KEEP_TELEMETRY)
static struct { const char *tag; unsigned us, ini, miss; } g_boot_mark[BOOT_MARKS];
static int g_boot_marks;
static uint64_t g_boot_t0;

static void boot_mark(const char *tag)
{
   if (g_boot_marks < BOOT_MARKS)
   {
      g_boot_mark[g_boot_marks].tag = tag;
      g_boot_mark[g_boot_marks].us =
         (unsigned)(sceKernelGetSystemTimeWide() - g_boot_t0);
      fe_ini_stats(&g_boot_mark[g_boot_marks].ini,
                   &g_boot_mark[g_boot_marks].miss);
      g_boot_marks++;
   }
}

static void boot_timeline_emit(void)
{
   char line[400];
   int i, o = 0;
   for (i = 0; i < g_boot_marks && o < (int)sizeof(line) - 40; i++)
      o += snprintf(line + o, sizeof(line) - (size_t)o, " %s=%u/%u/%u",
                    g_boot_mark[i].tag, g_boot_mark[i].us,
                    g_boot_mark[i].ini, g_boot_mark[i].miss);
   fe_evt("boot_timeline us/opens/miss:%s doc_hits=%u", line,
          fe_ini_doc_hits());
}
#else
/* This arm's definitions of BOOT_T0_SET(), boot_mark(), boot_timeline_emit()
 * are in fe/fe_priv.h. */
#endif

#ifdef GPSP_PERF_RIG
/* Harness `ui_script = <file>` (docs/GBA-LINK-UI.md): the in-game menus and
 * the LINKING screen driven step by step, for the two-PPSSPP link rig and
 * the GE dumps of every link screen.  `pad_script` is keyed to emulated
 * frames, which stop while a menu is open or a link sets up; this one runs
 * in order, each step `+N` main-loop iterations after the previous:
 *     +N menu               open the in-game menu (the chord is off with a
 *                           script driving the game)
 *     +N press BTN[+BTN]    pressed for one loop (OR'd into the pad)
 *     +N dump NAME          GE dump of this loop's frame -> log/NAME.bmp
 *     +N wait COND          until COND: linked linking hosting ended
 *                           failscreen ap_done net_up menu_open menu_closed
 *     +N exit CODE
 *     +N linking PRESET     the LINKING screen from fabricated facts
 *                           (uiscript_linking_draw), until `linking off`
 *     +N fakescan G1,G2,..  the scan list answers with these ("-" none)
 *     +N census TAG         heap_census(TAG): EVT heap_census at=TAG
 *                           (docs/UI-OVERLAY.md section 13's heap proof)
 * THE PLAYER'S ROUTE (docs/GBA-LINK-UI.md section 9), fixtures that read the
 * screen.  TEXT is "quoted", "A|B" = either; TO_S is wall seconds; WHY names
 * the failure (EVT route_fail, exit 8):
 *     +N chord              SELECT+START through the pad until the menu opens
 *     +N see TEXT TO_S WHY  until TEXT is drawn
 *     +N pick TEXT WHY [nopress]  DOWN to the highlighted row holding TEXT,
 *                           then X; a list that wraps round fails WHY
 *     +N watch TEXT WHY     from now on TEXT on screen fails WHY
 *     +N unwatch            none of those any more
 *     +N wait COND TO_S WHY as `wait`, with a limit
 *     +N route NAME         EVT route_step: the ladder the Lab scores
 *     +N label NAME / goto NAME
 *     +N ifsee TEXT LABEL MAX WHY  TEXT in this frame: jump, MAX times, then
 *                           fail WHY
 *     +N release            the game's script may start (adapter rooms)
 *     +N fail WHY
 * '#' comments.  Not compiled into a release. */
#define UISCRIPT_MAX 192
#define UISCRIPT_WATCH_MAX 24
#define UISCRIPT_DRIVES (g_uis_n != 0)
enum { UIS_MENU = 1, UIS_PRESS, UIS_DUMP, UIS_WAIT, UIS_EXIT, UIS_LINKING,
       UIS_FAKESCAN, UIS_CENSUS, UIS_CHORD, UIS_SEE, UIS_PICK, UIS_WATCH,
       UIS_UNWATCH, UIS_ROUTE, UIS_LABEL, UIS_IFSEE, UIS_RELEASE, UIS_FAIL,
       UIS_GOTO };
static struct
{
   unsigned delay, btn, to_s;
   unsigned char verb;
   short jumps;
   char arg[64], lbl[16], why[24];
} g_uis[UISCRIPT_MAX];
static struct { char text[48], why[24]; } g_uis_watch[UISCRIPT_WATCH_MAX];
static int g_uis_n, g_uis_i, g_uis_exit = -1, g_uis_watches;
static unsigned g_uis_wait;
static unsigned long long g_uis_t0;  /* the step began (us) */
static char g_uis_at[24] = "start";  /* the last `route` mark reached */
/* pick: look at the band, press, let it land, look again */
static int g_uis_pick_phase, g_uis_pick_moves;
static char g_uis_pick_first[32], g_uis_pick_prev[32];
static char g_uis_dump[24];
static char g_uis_linking[24];     /* `linking PRESET` until `linking off` */

/* "+N verb [arg] [a2] [a3] [a4]": an argument may be "quoted, with spaces".
 * Returns how many of verb, arg, ... + the +N were read. */
static int uiscript_tokens(const char *p, unsigned *d, char *verb, char *arg,
                           char *a2, char *a3, char *a4)
{
   char *out[5];
   size_t cap[5];
   int i, n = 0;
   if (*p == '#' || sscanf(p, " +%u", d) != 1)
      return 0;
   out[0] = verb; out[1] = arg; out[2] = a2; out[3] = a3; out[4] = a4;
   cap[0] = 16; cap[1] = 64; cap[2] = cap[3] = cap[4] = 24;
   while (*p == ' ' || *p == '\t')
      p++;
   while (*p && *p != ' ' && *p != '\t')
      p++;                                /* the +N */
   for (i = 0; i < 5; i++)
   {
      char end = ' ';
      size_t k = 0;
      while (*p == ' ' || *p == '\t')
         p++;
      if (!*p || *p == '\r' || *p == '\n')
         break;
      if (*p == '"')
      {
         end = '"';
         p++;
      }
      while (*p && *p != end && *p != '\r' && *p != '\n' &&
             (end == '"' || *p != '\t'))
      {
         if (k + 1 < cap[i])
            out[i][k++] = *p;
         p++;
      }
      out[i][k] = '\0';
      if (*p == end)
         p++;
      n++;
   }
   return n + 1;
}

/* A fixture's route ends here, by name: the step, the reason, what the
 * screen said (route_screen lines), a picture of it, an exit the Lab sees. */
static void uiscript_fail(const char *why, const char *detail)
{
   char path[176];
   FE_EVT_ONLY(detail);
   fe_evt("route_fail at=%s reason=%s step=%d detail=\"%s\" frame=%u",
          g_uis_at, why, g_uis_i, detail ? detail : "",
          fe_host_frame_count());
   vid_seen_report();
#ifndef GPSP_NO_TELEMETRY
   heap_census("route_fail");
#endif
   snprintf(path, sizeof(path), "%s/route_fail.bmp", LOG_DIR);
   vid_dump_ge(path);
   g_uis_exit = 8;
   g_uis_i = g_uis_n;
}

static void uiscript_load(const char *path)
{
   FILE *f = fopen(path, "r");
   char line[160];
   if (!f)
   {
      fe_evt("ui_script err=open path=%s", path);
      return;
   }
   while (fgets(line, sizeof(line), f) && g_uis_n < UISCRIPT_MAX)
   {
      char verb[16], arg[64];
      unsigned d;
      char a2[24] = "", a3[24] = "", a4[24] = "";
      int n = uiscript_tokens(line, &d, verb, arg, a2, a3, a4);
      if (line[0] == '#' || n < 2)
         continue;
      memset(&g_uis[g_uis_n], 0, sizeof(g_uis[0]));
      g_uis[g_uis_n].delay = d;
      if (n >= 3)
         snprintf(g_uis[g_uis_n].arg, sizeof(g_uis[0].arg), "%.63s", arg);
      if (!strcmp(verb, "chord"))
         g_uis[g_uis_n].verb = UIS_CHORD;
      else if (!strcmp(verb, "see") && n == 5)
      {
         g_uis[g_uis_n].verb = UIS_SEE;
         g_uis[g_uis_n].to_s = (unsigned)atoi(a2);
         snprintf(g_uis[g_uis_n].why, sizeof(g_uis[0].why), "%s", a3);
      }
      else if (!strcmp(verb, "pick") && (n == 4 || n == 5))
      {
         g_uis[g_uis_n].verb = UIS_PICK;
         snprintf(g_uis[g_uis_n].why, sizeof(g_uis[0].why), "%s", a2);
         g_uis[g_uis_n].btn = !strcmp(a3, "nopress") ? 0 : PSP_CTRL_CROSS;
      }
      else if (!strcmp(verb, "watch") && n == 4)
      {
         g_uis[g_uis_n].verb = UIS_WATCH;
         snprintf(g_uis[g_uis_n].why, sizeof(g_uis[0].why), "%s", a2);
      }
      else if (!strcmp(verb, "unwatch"))
         g_uis[g_uis_n].verb = UIS_UNWATCH;
      else if (!strcmp(verb, "route") && n == 3)
         g_uis[g_uis_n].verb = UIS_ROUTE;
      else if (!strcmp(verb, "label") && n == 3)
         g_uis[g_uis_n].verb = UIS_LABEL;
      else if (!strcmp(verb, "ifsee") && n == 6)
      {
         g_uis[g_uis_n].verb = UIS_IFSEE;
         snprintf(g_uis[g_uis_n].lbl, sizeof(g_uis[0].lbl), "%s", a2);
         g_uis[g_uis_n].to_s = (unsigned)atoi(a3);
         snprintf(g_uis[g_uis_n].why, sizeof(g_uis[0].why), "%s", a4);
      }
      else if (!strcmp(verb, "release"))
         g_uis[g_uis_n].verb = UIS_RELEASE;
      else if (!strcmp(verb, "fail") && n == 3)
         g_uis[g_uis_n].verb = UIS_FAIL;
      else if (!strcmp(verb, "goto") && n == 3)
         g_uis[g_uis_n].verb = UIS_GOTO;
      else if (!strcmp(verb, "menu"))
         g_uis[g_uis_n].verb = UIS_MENU;
      else if (!strcmp(verb, "press") && n == 3 &&
               (g_uis[g_uis_n].btn = pad_script_buttons(arg)) != 0)
         g_uis[g_uis_n].verb = UIS_PRESS;
      else if (!strcmp(verb, "dump") && n == 3)
         g_uis[g_uis_n].verb = UIS_DUMP;
      else if (!strcmp(verb, "wait") && (n == 3 || n == 5))
      {
         g_uis[g_uis_n].verb = UIS_WAIT;
         g_uis[g_uis_n].to_s = (unsigned)atoi(a2);
         snprintf(g_uis[g_uis_n].why, sizeof(g_uis[0].why), "%s", a3);
      }
      else if (!strcmp(verb, "exit"))
         g_uis[g_uis_n].verb = UIS_EXIT;
      else if (!strcmp(verb, "linking") && n == 3)
         g_uis[g_uis_n].verb = UIS_LINKING;
      else if (!strcmp(verb, "fakescan") && n == 3)
         g_uis[g_uis_n].verb = UIS_FAKESCAN;
      else if (!strcmp(verb, "census") && n == 3)
         g_uis[g_uis_n].verb = UIS_CENSUS;
      else
      {
         /* a step nobody understood is a fixture that lies: say so, end it */
         fe_evt("route_fail at=load reason=script_error step=%d detail=\"%s\"",
                g_uis_n, verb);
         g_uis_exit = 8;
         continue;
      }
      g_uis_n++;
   }
   fclose(f);
   fe_evt("ui_script path=%s steps=%d", path, g_uis_n);
}

static int uiscript_cond(const char *c)
{
   int st = fe_host_gblink_active() ? fe_host_gblink_state() : -1;
   if (!strcmp(c, "linked"))     return st == FE_GBLINK_RUNNING;
   if (!strcmp(c, "play"))       return !g_autoload_state &&
                                        fe_host_frame_count() >= 60;
   if (!strcmp(c, "started"))    return st >= 0;
   if (!strcmp(c, "settled"))    return st == FE_GBLINK_RUNNING ||
                                        g_link_fail_hold;
   if (!strcmp(c, "linking"))    return st == FE_GBLINK_SETUP;
   if (!strcmp(c, "hosting"))    return g_net_up && g_link_wait;
   if (!strcmp(c, "ended"))      return st < 0 && !g_net_up;
   if (!strcmp(c, "failscreen")) return g_link_fail_hold;
   if (!strcmp(c, "ap_done"))    return fe_autopilot_status() == 1;
   if (!strcmp(c, "net_up"))     return g_net_up;
   if (!strcmp(c, "menu_open"))  return ui_active();
   if (!strcmp(c, "menu_closed")) return !ui_active();
   return 0;
}

static void uiscript_jump(const char *label)
{
   int t;
   for (t = 0; t < g_uis_n; t++)
      if (g_uis[t].verb == UIS_LABEL && !strcmp(g_uis[t].arg, label))
      {
         g_uis_i = t;
         return;
      }
   uiscript_fail("script_error", label);
}

/* At the pad read: the buttons to OR in this loop. */
static unsigned uiscript_pad(void)
{
   unsigned m = 0;
   int adv = 1, k;
   unsigned long long now = sceKernelGetSystemTimeWide();
   vid_seen_frame();
   if (g_uis_i >= g_uis_n)
   {
      vid_seen_enable(0);            /* the session's frames pay nothing */
      return 0;
   }
   /* Text is read only while something needs it: a session's own frames must
    * not pay for it. */
   vid_seen_enable(g_uis_watches > 0 || g_uis[g_uis_i].verb == UIS_SEE ||
                   g_uis[g_uis_i].verb == UIS_PICK ||
                   g_uis[g_uis_i].verb == UIS_IFSEE ||
                   g_uis[g_uis_i + 1 < g_uis_n ? g_uis_i + 1 : g_uis_i].verb ==
                      UIS_IFSEE);
   for (k = 0; k < g_uis_watches; k++)
      if (vid_seen_has(g_uis_watch[k].text))
      {
         uiscript_fail(g_uis_watch[k].why, g_uis_watch[k].text);
         return 0;
      }
   if (++g_uis_wait < g_uis[g_uis_i].delay)
      return 0;
   if (!g_uis_t0)
      g_uis_t0 = now;
   if (g_uis[g_uis_i].to_s && g_uis[g_uis_i].verb != UIS_IFSEE &&
       now - g_uis_t0 > (unsigned long long)g_uis[g_uis_i].to_s * 1000000ull)
   {
      uiscript_fail(g_uis[g_uis_i].why[0] ? g_uis[g_uis_i].why : "timeout",
                    g_uis[g_uis_i].arg);
      return 0;
   }
   switch (g_uis[g_uis_i].verb)
   {
   case UIS_CHORD:
      /* The player's own chord, through the pad: held until the menu opens. */
      adv = ui_active();
      if (!adv)
      {
         m = PSP_CTRL_SELECT | PSP_CTRL_START;
         if (now - g_uis_t0 > 5000000ull)
         {
            uiscript_fail("menu_not_opening", "SELECT+START");
            return 0;
         }
      }
      break;
   case UIS_SEE:
      adv = vid_seen_has(g_uis[g_uis_i].arg);
      break;
   case UIS_PICK:
   {
      /* Down the list to the row, reading the band; a list that comes back
       * round, or a band that will not move, is a menu that changed. */
      const char *row = vid_seen_band_row();
      int on = vid_seen_band_has(g_uis[g_uis_i].arg);
      adv = 0;
      if (g_uis_pick_phase)
         g_uis_pick_phase--;
      else if (on < 0)
      {
         if (now - g_uis_t0 > 5000000ull)
         {
            uiscript_fail(g_uis[g_uis_i].why, g_uis[g_uis_i].arg);
            return 0;
         }
      }
      else if (on)
      {
         m = g_uis[g_uis_i].btn;
         adv = 1;
      }
      else
      {
         if (!g_uis_pick_moves)
            snprintf(g_uis_pick_first, sizeof(g_uis_pick_first), "%s", row);
         else if (!strcmp(row, g_uis_pick_first) ||
                  (g_uis_pick_moves > 40 && !strcmp(row, g_uis_pick_prev)))
         {
            uiscript_fail(g_uis[g_uis_i].why, g_uis[g_uis_i].arg);
            return 0;
         }
         snprintf(g_uis_pick_prev, sizeof(g_uis_pick_prev), "%s", row);
         g_uis_pick_moves++;
         m = PSP_CTRL_DOWN;
         g_uis_pick_phase = 3;
      }
      if (adv)
      {
         g_uis_pick_moves = 0;
         g_uis_pick_phase = 0;
      }
      break;
   }
   case UIS_WATCH:
      if (g_uis_watches < UISCRIPT_WATCH_MAX)
      {
         snprintf(g_uis_watch[g_uis_watches].text,
                  sizeof(g_uis_watch[0].text), "%s", g_uis[g_uis_i].arg);
         snprintf(g_uis_watch[g_uis_watches].why,
                  sizeof(g_uis_watch[0].why), "%s", g_uis[g_uis_i].why);
         g_uis_watches++;
      }
      break;
   case UIS_UNWATCH:
      g_uis_watches = 0;
      break;
   case UIS_ROUTE:
      snprintf(g_uis_at, sizeof(g_uis_at), "%s", g_uis[g_uis_i].arg);
      fe_evt("route_step name=%s t_ms=%llu frame=%u", g_uis_at,
             now / 1000ull, fe_host_frame_count());
      break;
   case UIS_LABEL:
      break;
   case UIS_IFSEE:
      /* decided on this frame's view: run it right after a `see` */
      if (vid_seen_has(g_uis[g_uis_i].arg))
      {
         if (g_uis[g_uis_i].jumps++ >= (short)g_uis[g_uis_i].to_s)
         {
            uiscript_fail(g_uis[g_uis_i].why, g_uis[g_uis_i].arg);
            return 0;
         }
         uiscript_jump(g_uis[g_uis_i].lbl);
      }
      break;
   case UIS_GOTO:
      uiscript_jump(g_uis[g_uis_i].arg);
      break;
   case UIS_RELEASE:
      fe_host_script_release();
      break;
   case UIS_FAIL:
      uiscript_fail(g_uis[g_uis_i].arg, "script");
      return 0;
   case UIS_MENU:
      if (!ui_active())
         menu_open();
      break;
   case UIS_PRESS:
      m = g_uis[g_uis_i].btn;
      break;
   case UIS_DUMP:
      snprintf(g_uis_dump, sizeof(g_uis_dump), "%.23s", g_uis[g_uis_i].arg);
      break;
   case UIS_WAIT:
      adv = uiscript_cond(g_uis[g_uis_i].arg);
      if (!adv && !strcmp(g_uis[g_uis_i].arg, "ap_done") &&
          fe_autopilot_status() == -1)
      {
         uiscript_fail("game_script", "ap_fail");
         return 0;
      }
      break;
   case UIS_EXIT:
      g_uis_exit = atoi(g_uis[g_uis_i].arg);
      break;
   case UIS_LINKING:
      snprintf(g_uis_linking, sizeof(g_uis_linking), "%.23s",
               strcmp(g_uis[g_uis_i].arg, "off") ? g_uis[g_uis_i].arg : "");
      break;
   case UIS_FAKESCAN:
      ui_scan_fake_set(g_uis[g_uis_i].arg);
      break;
   case UIS_CENSUS:
#ifndef GPSP_NO_TELEMETRY
      heap_census(g_uis[g_uis_i].arg);
#endif
      break;
   }
   if (adv)
   {
      fe_evt("ui_script step=%d verb=%u arg=%s loop=%u frame=%u", g_uis_i,
             (unsigned)g_uis[g_uis_i].verb, g_uis[g_uis_i].arg,
             (unsigned)g_loop_iter, fe_host_frame_count());
      g_uis_i++;
      g_uis_wait = 0;
      g_uis_t0 = 0;
   }
   return m;
}

/* Just before the present: this loop's frame, menus and OSD included. */
static void uiscript_dump_now(void)
{
   char path[176];
   if (!g_uis_dump[0])
      return;
   snprintf(path, sizeof(path), "%s/%s.bmp", LOG_DIR, g_uis_dump);
   if (vid_dump_ge(path) == 0)
      fe_evt("ge_dump file=%s.bmp ui_script=1", g_uis_dump);
   g_uis_dump[0] = '\0';
}
/* `linking PRESET`: the LINKING screen drawn from fabricated session facts
 * through the same composition a session uses (link_draw_facts,
 * link_bringup_view), for the GE comparison with the design's frames
 * (docs/GBA-LINK-UI.md).  Presets: share check gb_receive fail_rom
 * fail_version fail_host_left adapter_join adapter_host cable_join. */
static void uiscript_linking_draw(void)
{
   fe_gblink_progress pr;
   const char *p = g_uis_linking;
   int why = g_link_why, host = g_net_is_host;
   if (!g_link_room[0])
      snprintf(g_link_room, sizeof(g_link_room), "Room 7");
   memset(&pr, 0, sizeof(pr));
   pr.peer_reason = -1;
   pr.state = FE_GBLINK_SETUP;
   g_net_is_host = 0;
   if (!strncmp(p, "adapter_", 8) || !strcmp(p, "cable_join"))
   {
      ui_linking_view lv;
      char l1[80], l2[80];
      int k = g_link_kind;
      g_link_kind = p[0] == 'a' ? LK_ADAPTER : LK_CABLE;
      link_bringup_view(!strcmp(p, "adapter_host"), g_link_room, &lv, l1,
                        sizeof(l1), l2, sizeof(l2));
      ui_linking_draw(&lv);
      g_link_kind = k;
      g_net_is_host = host;
      g_drew = 1;
      return;
   }
   if (!strcmp(p, "share"))
   {
      pr.phase = FE_GBLINK_PH_SHARE;
      pr.done = 180u * 1024u;
      pr.total = 412u * 1024u;
   }
   else if (!strcmp(p, "check"))
      pr.phase = FE_GBLINK_PH_HELLO;
   else if (!strcmp(p, "gb_receive"))
   {
      pr.phase = FE_GBLINK_PH_ROM_RX;
      pr.done = 1258291u;
      pr.total = 2097152u;
      snprintf(pr.peer_title, sizeof(pr.peer_title), "Pokemon Crystal");
   }
   else
   {
      pr.state = FE_GBLINK_FAILED;
      pr.phase = FE_GBLINK_PH_FAILED;
      pr.error = !strcmp(p, "fail_rom") ? FE_GBLINK_E_ROM_MISSING
                                        : FE_GBLINK_E_PEER_LOST;
      if (!strcmp(p, "fail_version"))
         g_link_why = LW_VERSION;
   }
   link_draw_facts(&pr);
   g_link_why = why;
   g_net_is_host = host;
}
#define UISCRIPT_DUMP() uiscript_dump_now()
#else
#define UISCRIPT_DUMP() do { } while (0)
#define UISCRIPT_DRIVES 0
#endif


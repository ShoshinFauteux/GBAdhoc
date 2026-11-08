static me_capture_frame *g_mer_cap[2];
/* One capture's slot in the volatile carve, 64-byte aligned.  This was the
 * literal 20544 (the pre-ME_AFFINE_LINES struct, 20500 B, rounded up); a
 * literal cannot follow the struct, and a struct that outgrew it would have
 * had capture 0 overwrite the start of capture 1 on hardware only. */
#define MER_CAP_BYTES ((int)((sizeof(me_capture_frame) + 63u) & ~63u))
static uint16_t *g_mer_stage[2];
#ifdef REWIND
/* THE REWIND'S PICTURES ARE THE ENGINE'S OWN OUTPUT (docs/REWIND.md, 10.6).
 * A stage is 256 x 161 pixels, and so is a slot of the rewind's picture
 * windows.  While the rewind records, each frame the engine draws is drawn
 * straight into that frame's slot: g_mer_stage[out] is pointed at it for
 * the post (me_rend_fill_desc) and everything that presents or reads "the
 * ready stage" follows the pointer.  No copy on either CPU.
 *   g_mer_stage_own  the two stages the engine carved, used whenever a
 *                    frame has no slot
 *   g_mer_priv[i]    stage i is being drawn for a frame RE-RUN by the
 *                    rewind: when it retires it is not promoted to
 *                    "ready", because nobody is to see it yet
 *   g_rw_slot        the slot of the frame being emulated (NULL: none);
 *                    set by the rewind before the frame, consumed by the
 *                    post
 *   g_rw_flush_posts slots this CPU may still hold cache lines for (it
 *                    filled them itself while the engine was down, or the
 *                    memory is freshly carved): the next posts write back
 *                    and drop their slot's lines first. */
static uint16_t *g_mer_stage_own[2];
static int       g_mer_priv[2];
static uint16_t *g_rw_slot;
static int       g_rw_slot_posted;
/* measurement: a drawn re-run frame and the second core (EVT rewind_me) */
static struct
{
   unsigned posts, post_us, retire_us, busy, show_waits, show_us;
} g_rwm;
static int g_rw_ring = 2;       /* which ring (psp_rewind_hud) */
static int       g_rw_flush_posts;
#endif
static me_render_desc *g_mer_desc;
/* Engine buffers live in the PSP's VOLATILE MEMORY when available — the 4 MB
 * OS pool at 0x08400000 that games may lock and gpsp never touches.  Zero
 * main-heap footprint, so np_start never contends with the engine: the host
 * heap has NO margin (np_start passed with the engine fully freed, run 411,
 * and failed with just the 41 KB captures resident, run 415-era).  With
 * volatile backing the suspend/resume dance is unnecessary and skipped.
 * Fallback when the lock fails: main heap + the suspend/resume path. */
static int g_mer_vmem;
#ifdef REWIND
/* THE POOL HAS A SECOND TENANT IN A REWIND BUILD (docs/REWIND.md, "memory").
 *
 * The engine carves its buffers from the START of the 4 MiB pool upwards and
 * uses well under a megabyte.  The rewind (its reference image, its ring, its
 * buffers) lives at the END, carved downwards by fe_host.  The boundary is
 * fixed: the first PSP_VM_ENGINE_BYTES belong to the engine, whatever it uses
 * of them, and me_rend_init checks that it stayed inside (psp_rewind_pool_check
 * takes the rewind out of the pool if it ever did not).
 *
 * One lock, taken by whichever tenant comes first (vm_lock is idempotent) and
 * given back only on the way into a sleep, by me_rend_release_vmem, after the
 * rewind has been told to let go.  The pool does not survive a sleep: the
 * rewind's history starts again after a wake. */
#define PSP_VM_ENGINE_BYTES (1024 * 1024)
static void *g_vm_base;
static int   g_vm_size, g_vm_locked;
static unsigned g_vm_eng_top;            /* end of what the engine carved */
/* A third tenant, harness-only for now (rom_vmem, docs/MEMORY-BUDGET.md): with
 * the rewind off, the same part of the pool as ROM cache pages. */
static int   g_rom_vm_lend, g_rom_vm_asleep;
static void  psp_rewind_pool_back(void);
static void  psp_rewind_pool_lost(void);
static void  psp_rewind_pool_check(void);

static int vm_lock(void **base, int *size)
{
   if (!g_vm_locked)
   {
      void *b = NULL;
      int s = 0;
      if (sceKernelVolatileMemTryLock(0, &b, &s) != 0)
         return -1;
      g_vm_base = b;
      g_vm_size = s;
      g_vm_locked = 1;
   }
   *base = g_vm_base;
   *size = g_vm_size;
   return 0;
}

static void vm_unlock(void)
{
   if (g_vm_locked)
      sceKernelVolatileMemUnlock(0);
   g_vm_locked = 0;
}

/* The engine found the pool unusable: give it back unless the rewind is in
 * it. */
static void vm_unlock_engine(void)
{
   if (!psp_rewind_pool_tenant())
      vm_unlock();
}
#else
/* This arm's definitions of MER_VM_TRYLOCK(), MER_VM_REJECT(), MER_VM_UNLOCK()
 * are in fe/fe_priv.h. */
#endif
/* THE ENGINE'S WORK AREA (me/me_mbox.h): the renderer's VRAM mirror, OBJ lists
 * and frame buffer, 278 KB that used to be static arrays in the kernel module
 * and is why that module did not load on a PSP-1000 short of kernel memory.
 * Carved from the same place as everything above and named in every render
 * desc.
 *
 * void * ON PURPOSE: this CPU never reads or writes the area.  The ME works
 * on it through its own cache, so anything read here would be stale and
 * anything written here would be lost or, worse, written back over the ME's
 * data later.  me_rend_init drops whatever lines this CPU's cache held for
 * the range, once, and from then on the address is only ever handed to the
 * ME.
 *
 * `gen` counts renderer builds.  The area's contents do not survive one (the
 * pool goes back to the OS across a sleep), and the module can stay loaded
 * right through it, so the ME is told when to stop believing them. */
static void    *g_mer_work;
static unsigned g_mer_work_gen;
/* `me_vmem` (harness only, default 1): 0 skips the volatile pool so the heap
 * fallback below -- and with it ME_CMD_SYNC, which only that path needs --
 * can be run on purpose instead of waiting for a console where the lock
 * fails. */
static int g_mer_vmem_cfg = 1;
static int g_mer_cur;                    /* capture buffer the core fills */
static int g_mer_pending = -1;           /* stage being rendered by the ME */
static int g_mer_ready   = -1;           /* stage ready to present */
static int g_mer_last_presented = -1;
static unsigned g_mer_watch_us;
static int g_vhash_on;                   /* harness oracle enabled */
/* harness vhash_legacy_rb = 1: hash each pixel with its red and blue fields
 * exchanged, i.e. in the libretro RGB565 order the core's buffer had until
 * ADR-0039 (594837a, 2026-08-03) made it the GE's 5650.  Only for comparing
 * a run with a golden captured before that commit. */
static int g_vhash_legacy_rb;
static void mer_note_sum(void);          /* defined below vhash_frame */
static unsigned g_mer_frames, g_mer_drops, g_mer_miss;
static unsigned g_mer_wait_us, g_mer_wait_max, g_mer_census;
/* ---- INPUT SHADOWS (kills the per-frame input spin) ---------------------
 * The ME's phase 1 memcpy's vram+oam+palette out of MAIN RAM into its own
 * eDRAM, and the main CPU SPINS for all of it -- measured 1415-1456 us EVERY
 * frame on both consoles (EVT me_rend wait_mean_us, drops=0, so this is
 * handshake latency and not the renderer being late).  It waits because the
 * moment emulation resumes it starts writing those same arrays underneath
 * the ME.
 *
 * Give the ME its own stable copy instead.  The main CPU snapshots the three
 * arrays into volatile-memory shadows, points the descriptor at those, and
 * resumes immediately -- there is nothing left for the ME to race against.
 * A cached main-RAM memcpy of 98 KB costs far less than 1.45 ms of spinning
 * on an UNCACHED read at ~67 MB/s, which is what we are paying today.
 *
 * Safe with a single (not double) shadow because me_rend_fill_desc() is only
 * ever reached after step 1 has confirmed me_host_idle() -- the ME has
 * finished the whole previous render, so it long ago finished reading its
 * inputs.  Refilling cannot overtake a reader that does not exist.
 *
 * Volatile-backed only.  The heap fallback does NOT get shadows: a 96 KB
 * malloc already fails on the host after np_start (observed run 418,
 * `vram_probe off reason=alloc`), and quietly running the fast path on one
 * console and the slow path on the other is exactly the split that makes a
 * two-console measurement meaningless.  Off = today's behaviour exactly. */
static unsigned char *g_mer_sh_vram, *g_mer_sh_oam, *g_mer_sh_pal;
static int      g_mer_shadow;         /* wanted (harness key `me_shadow`)  */
static int      g_mer_sh_on;          /* actually allocated + in use       */
static unsigned g_mer_sh_us, g_mer_sh_max;
/* ME RENDER DURATION -- the number the whole low-latency question turns on.
 * The "~10 ms render" quoted in comments here was never measured. Uncapped
 * smooth FF reaching 120+ fps implies <=8.3 ms, and that is a LOWER bound on
 * the ME's speed: smooth FF renders every frame, so the ME can never outrun
 * the rate the CPU feeds it (90 fps in battles is the CPU saturating, not
 * the ME slowing).
 *
 * If the render is ~8 ms, handing the capture over at vcount==160 -- where
 * it is already COMPLETE, 68 scanlines before we currently post it -- gives
 * a 3.3 ms head start and leaves a ~4.7 ms wait, i.e. a ~15.7 ms frame that
 * still fits 16.7. Chunking would be unnecessary. If it is ~10 ms it does
 * not fit and the pipeline has to be split. One measurement, two very
 * different designs.
 *
 * Sampled in the vblank wait, which is idle anyway. A render that outlasts
 * the wait yields no sample, so `n` being small is itself informative --
 * it means the render is longer than the frame's slack. */
static unsigned g_mer_post_us, g_mer_rend_n, g_mer_rend_us, g_mer_rend_max;
#if defined(FX_SWEEP) && defined(GPSP_PERF_RIG)
static unsigned g_fxs_rn, g_fxs_ru;    /* the cost sweep's own ME sums */
#endif
/* WHEN IS THE CAPTURE ACTUALLY READY?
 *
 * The plan to present a frame in its OWN frame hinges on handing the ME its
 * capture at vcount==160, where it is already complete, instead of at the end
 * of retro_run 68 scanlines later. The size of the ME's window is then
 * (frame budget - GE blit) minus whenever line 160 falls.
 *
 * It is tempting to assume 160/228 of the frame -- 70% -- but CPU time is not
 * proportional to scanlines. VBlank is where games run their logic, AI,
 * sprite updates and librfu's exchange, so lines 160-227 plausibly cost far
 * more than 30% of the time. If line 160 actually lands at ~40% of the frame
 * the ME gets ~10.7 ms rather than ~7.5 ms, which is the difference between
 * the idea fitting comfortably and not fitting at all.
 *
 * Measured against the frame's own start (frame_t0), so it reads directly as
 * "the capture was ready N us into the frame". */
static unsigned g_vis_n, g_vis_us, g_vis_max;
/* The ME keeps a persistent eDRAM mirror of VRAM and we now patch it with
 * dirty pages only. That is only valid if the mirror was ever correct, so
 * force a full copy on the first frame and after any teardown/resume. */
static int g_mer_full_copy = 1;
/* `me_dirty` -- 1 = copy only dirty VRAM pages, 0 = always copy all 96 KB.
 *
 * Exists so ONE binary produces both arms of the correctness test. Rebuilding
 * the previous build to get baseline frame hashes would compare two binaries
 * that differ in more than this; with the switch, everything except the copy
 * strategy is byte-identical, so any frame-hash difference is attributable. */
static int g_mer_dirty_cfg = 1;
/* `me_capmode` (harness only, default 1): the core's me_capture_mode while the
 * engine is up.  2 = capture AND render on this CPU as well (the desktop
 * validation mode); the ME still renders and presents exactly as in 1.  It
 * exists for ONE experiment: it restores the CPU-side frame cost and cache
 * pressure of ME-off while keeping the ME, its PRX, its bus traffic and its
 * handshakes -- so a failure that follows `me_mode` can be split into "the
 * second core" versus "what the second core does to this CPU's timing". */
static unsigned g_mer_capmode = 1;
/* SAME-FRAME PRESENTATION (`me_sameframe`, default 0).
 *
 * Today the capture is handed to the ME at the END of retro_run and the
 * result is shown at the top of the NEXT frame -- one frame, 16.7 ms, of
 * pure display latency. But the capture is COMPLETE at vcount 160
 * (ioregs[0..159] are all written by then); we simply sit on it for another
 * 68 scanlines of VBlank.
 *
 * Post it the moment it exists and the render lands in idle the CPU already
 * had. Measured: capture ready 7.0 ms, ME render 3.2 ms, GE blit 1.5 ms =
 * 11.7 ms of a 16.74 ms frame -- and the dirty-page copy widened that margin
 * further by collapsing the ME's input phase from 1.45 ms to 0.15 ms.
 *
 * If the ME misses the deadline we present the PREVIOUS frame, which is
 * exactly today's behaviour for that frame only: never a stutter, never a
 * dropped emulated frame. The failure mode degrades to the shipping design
 * rather than to something worse. */
static int      g_mer_sameframe;      /* configured */

/* Same-frame retirement stays off throughout FF. Each profile has ONE
 * producer (3x: end of frame; the gated presets: the vcount-160 post, as at
 * 1x) and loop-top presentation; a second producer would corrupt
 * stage/capture ownership. */
static int mer_sameframe_active(void)
{
   return g_mer_sameframe && !g_ff_mult && !g_ff_uncapped;
}


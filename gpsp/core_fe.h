/* core_fe.h -- what the core exports to a native frontend (tier C, C-6;
 * builds/health-out/design/C-6.md).
 *
 * One declaration per core name a frontend reads or calls, in the core's own
 * types, grouped by the file that defines it.  common.h includes this header
 * (after its type block and the standard headers, before cpu.h), so every core
 * TU compiles its definition against the one declaration here: a mismatch is a
 * build error in every flavour and every port.  A frontend includes it next to
 * its own headers instead of copying declarations by hand.
 *
 * DEFINE-FREE (ci_greps R8 core_fe_pure): the only conditionals are the include
 * guard and __cplusplus.  No attribute, no `static`, no definition: a
 * declaration that carried section(".sbss") would make frontend code address
 * the variable through $gp, and a `weak` one would turn the core's definition
 * weak.  A name that exists only in some builds is still declared plainly; the
 * frontend that tests its address uses `#pragma weak NAME` beside the use.
 *
 * The includer provides the types: u8/u16/u32/s64 (psptypes.h on the PSP,
 * common.h's type block elsewhere), time_t (<time.h>) and bool (<stdbool.h>;
 * native in C++).  RFILE is libretro-common's `typedef struct RFILE RFILE`, so
 * it is named as a struct.
 *
 * NOT HERE, on purpose:
 *   - rom_translation_cache, rom_translation_cache_size, ram_translation_cache_size:
 *     their type depends on the core's configuration (cpu.h: a pointer under
 *     RUNTIME_JIT_CACHE/MMAP_JIT_CACHE/VITA, an array otherwise, a macro on 3DS),
 *     so a define-free header cannot declare them.  cpu.h stays their home.
 */
#ifndef CORE_FE_H
#define CORE_FE_H

#ifdef __cplusplus
extern "C" {
#endif

struct RFILE;

/* ---- main.c ------------------------------------------------------------- */
extern u32 flush_rom_total;
extern u32 flush_ram_full;
extern u32 flush_ram_smc;
extern u32 flush_ram_dma;
extern u32 smc_blk_watch;
extern u32 smc_blk_xlat_total;
extern u32 smc_flush_us;
extern u32 smc_flush_bytes;
extern u32 smc_flush_wide;
extern u32 (*smc_prof_clock)(void);
extern u32 cph_jit_total;
extern u32 sched_fast;              /* mips_stub.S under SCHED_HOT_IN_REG */
extern u32 timer_lazy;              /* HALT_HOP (built in) / TIMER_LAZY mask */
void smc_prof_take(unsigned *hits, unsigned *pages, unsigned *iw, unsigned *ew,
                   unsigned *oth, unsigned *ovf, unsigned *hot_addr, unsigned *hot_cnt,
                   unsigned *top_addr, unsigned *top_cnt, unsigned nhot);
void smc_blk_take(unsigned *watch, unsigned *xlat, unsigned *ovf,
                  unsigned *start, unsigned *end, unsigned *mode, unsigned *reason,
                  unsigned *cnt, unsigned *wpc, unsigned *waddr, unsigned *wcnt,
                  unsigned *wovf, unsigned *head, unsigned *ctx, unsigned *snap,
                  unsigned *fus, unsigned *fbytes, unsigned *fwide, unsigned *same,
                  unsigned *diff, unsigned *unkn);
void core_phase_set_level(unsigned lvl);
void core_phase_take(unsigned *lvl, unsigned *clk_ns, unsigned *frames,
                     unsigned *reads, unsigned *mean, unsigned *max, unsigned *worst,
                     unsigned nph, unsigned *bkr, unsigned *bkw, unsigned *wbkr,
                     unsigned *wbkw, unsigned *ugba, unsigned *dman, unsigned *stmr,
                     unsigned *negc, unsigned *nego, unsigned *rfux, unsigned *rfut,
                     unsigned *wrfux, unsigned *wrfut);

/* ---- cpu.cc (the GBA memory the ME renderer and the rig read) ----------- */
extern u32 reg[64];
extern u16 oam_ram[512];
extern u16 palette_ram[512];
extern u16 palette_ram_converted[512];
extern u16 io_registers[512];
extern u8 ewram[1024 * 256 * 2];
extern u8 iwram[1024 * 32 * 2];
extern u8 vram[1024 * 96];

/* ---- cpu_threaded.c (the dynarec) --------------------------------------- */
extern u32 jit_cache_tier;
extern u32 jit_cache_force_small;
extern const char *jit_cache_reason;
void dynarec_release_translation_caches(void);
void dynarec_heap_tables_release(void);
extern u32 rom_cache_watermark;
extern u32 irq_io_fast;
extern u32 msr_fast;
extern u32 platform_cache_paranoid;       /* PSP (and JITCOH_PSP_CACHE) builds */
u32 jit_coherency_scan(u32 *first);       /* PSP (and JITCOH_PSP_CACHE) builds */
extern u32 smc_fallback_stats[16];
extern u32 badjump_contained;
extern u32 badjump_recoveries;
extern u32 dma_smc_flushes;
extern u32 xlat_depth_max;                /* XLAT_DEPTH_PROBE builds */
extern u32 xlat_sp_min;                   /* XLAT_DEPTH_PROBE builds */
void xlat_depth_probe_reset(void);        /* XLAT_DEPTH_PROBE builds */
extern u32 xlat_scan_fast;                /* a constant (1u) in XLAT_FAST builds */
extern u32 jit_code_mode;                 /* JIT_CODE_AB builds */
extern u32 jit_pub_inline;
extern u32 translation_gate_targets;
extern u32 translation_gate_target_pc[];  /* MAX_TRANSLATION_GATES (cpu.h, GATE_SLOTS) */
void rom_hash_census(u32 out[10]);
/* JIT_CODE_DISCIPLINE statistics (jit_code.h, docs/JIT-CODE-DISCIPLINE.md): the lab
 * heartbeat reads them by field.  owner[] counts ownership changes by reason, jit_code.h's
 * JOWN_* order; jit_code.h asserts JOWN_COUNT == CORE_FE_JOWN_COUNT. */
#define CORE_FE_JOWN_COUNT 5
typedef struct
{
  u32 publishes;        /* non-empty publishes                               */
  u32 spans;            /* spans published                                   */
  u32 lines;            /* 64-byte lines written back (and, if ranged, I-inv) */
  u32 whole_i;          /* whole I-cache invalidates                         */
  u32 early;            /* publishes forced by a full span table             */
  u32 owner[CORE_FE_JOWN_COUNT];
  u32 big_i;            /* whole-I forced by a span >= 16 KiB                 */
  u32 kcalls;           /* kernel cache calls (ranged + whole) by publishes  */
  u32 lineops;          /* user-mode cache line ops (JIT_COST PUBINL)        */
} jit_code_stats_type;
extern jit_code_stats_type jit_code_stats;

/* ---- gba_memory.c ------------------------------------------------------- */
/* The VRAM dirty-page map (vram_clean[], 1 KiB pages; gba_memory.h builds the
 * rest of its layout on these two). */
#define VRAM_DIRTY_SHIFT 10
#define VRAM_DIRTY_PAGES ((1024 * 96) >> VRAM_DIRTY_SHIFT)
extern u8 vram_clean[];                   /* VRAM_CLEAN_ARRAY_BYTES (gba_memory.h) */
extern u32 vram_dirty_marks;
extern u32 gamepak_size;
extern u32 gamepak_buffer_count;
extern u32 gamepak_buffer_pages;
extern u32 gamepak_buffer_cap;
extern u32 gamepak_resident_wanted;
extern u32 gamepak_resident_got;
extern const char *gamepak_cap_reason;
extern u32 gamepak_static_blocks;
extern u32 gamepak_force_swap_stubs;
extern u32 gamepak_page_loads;
extern u32 gamepak_reopens;
extern u32 gamepak_short_reads;
bool gamepak_must_swap(void);             /* the cart pages: not all of it is resident */
extern u8 *gamepak_buffers[32];
extern u8 *gamepak_spare_pool3;
extern u32 gamepak_spare_pool3_bytes;
extern struct RFILE *gamepak_file_large;
void gamepak_vol_drop(void);
void gamepak_vol_back(void);
extern void (*gpsp_rom_load_progress)(u32 loaded, u32 total);
extern void (*gpsp_heap_census)(const char *where);
extern time_t (*gpsp_wallclock)(void);
extern volatile s64 gpsp_rtc_wall;
void gpsp_rtc_peek(long long *shown, long long *frames_time, int *wall);

/* ---- rfu.c (the AGB-015 wireless adapter) ------------------------------- */
void rfu_set_disc_grace(u32 frames);
void rfu_set_poll_min_cycles(u32 n);
void rfu_set_idle_poll_cycles(u32 n);
void rfu_set_timeout_scale(u32 to_x100, u32 rtx_x100);
void rfu_set_fault_corrupt(u32 n);
void rfu_set_fault_span(u32 n);
void rfu_set_fault_mode(u32 m);

/* ---- video.cc / video_me_log.h (the frame the frontend presents) -------- */
extern u16 *gba_screen_pixels;
extern int me_log_vram_always;
const u16 *me_log_line160_pal(void);
const u16 *me_log_line160_oam(void);

/* ---- gba_cc_lut.c ------------------------------------------------------- */
extern const u16 gba_cc_lut[];

/* ---- libretro/libretro.c ------------------------------------------------ */
extern u32 skip_next_frame;

#ifdef __cplusplus
}
#endif

#endif /* CORE_FE_H */

/* video_prof.h — renderer path profiler for video.cc (ADR-0034).
 *
 * ADR-0032 bracketed retro_run and found the software scanline renderer is
 * one of the two largest phases of the core's frame.  It did NOT say which
 * renderer paths that time belongs to, and video.cc has ~20 of them (tiled
 * text / mosaic / affine backgrounds, 4bpp vs 8bpp, sprites regular vs affine
 * vs mosaic, four color-effect modes, eight window configurations, three
 * bitmap modes).  Optimising the wrong one is the failure mode this header
 * exists to prevent.
 *
 * Design, deliberately the same discipline as ADR-0032:
 *   - COUNTERS are exact and free of distortion: calls and PIXELS per path.
 *     They are hardware-independent, so they transfer from the rig to a PSP
 *     unchanged, which timings do not.
 *   - CLOCKS are levelled and self-priced.  Every clock read is counted in
 *     VP_RD, so `VP_RD x ns-per-read` prices the probe in its own units and
 *     a level A/B measures the cost instead of asserting it.
 *   - Nested brackets are guarded: only the OUTERMOST active bracket of a
 *     given bucket accumulates, so a path that calls another timed path
 *     cannot be counted twice (the hazard that made ADR-0032's first cut
 *     report a 4.5x-too-large JIT cost).
 *
 * Entirely compiled out unless -DVIDEO_PROF=1: every macro below becomes
 * `do {} while (0)` and no storage is emitted.  Nothing in video.cc's
 * behaviour changes in either configuration — the counters are written, never
 * read, by the renderer.
 */

#ifndef VIDEO_PROF_H
#define VIDEO_PROF_H

#ifndef VIDEO_PROF
#define VIDEO_PROF 0
#endif

/* Bucket ids.  Times are microseconds accumulated over one frame; counts are
 * per frame.  Order must match video_prof_names[] in video.cc. */
enum {
  /* --- times (us) --- */
  VP_T_TOTAL,     /* whole update_scanline(), 160 calls a frame           */
  VP_T_BG,        /* tiled/bitmap BG layer rendering (excl. sprites)      */
  VP_T_OBJ,       /* render_scanline_objs()                               */
  VP_T_MERGE,     /* merge_blend() + merge_brightness() passes            */
  VP_T_FILL,      /* fill_line_background() + render_backdrop()           */
  /* --- structural counts --- */
  VP_C_LINES,     /* update_scanline() calls (160/frame when not skipped) */
  VP_C_MODE0, VP_C_MODE1, VP_C_MODE2, VP_C_MODE3, VP_C_MODE4, VP_C_MODE5,
  VP_C_WIN_NONE,  /* lines with no window active (win_ctrl == 0)          */
  VP_C_WIN_N,     /* lines using window 0/1                               */
  VP_C_WIN_OBJ,   /* lines using the obj-window                           */
  VP_C_EFF_NONE, VP_C_EFF_BLEND, VP_C_EFF_BRIGHT, VP_C_EFF_DARK,
  VP_C_OBJBLEND,  /* render_w_effects calls with semi-transparent objects */
  /* --- per-path calls and PIXELS (pixels are the unit that matters) --- */
  VP_C_TEXT_CALL, VP_C_TEXT_PX,     /* non-mosaic tiled text BG           */
  VP_C_TEXT8_CALL,                  /* ...of which 8bpp                   */
  VP_C_MOSAIC_CALL, VP_C_MOSAIC_PX, /* mosaic tiled text BG               */
  VP_C_AFF_CALL, VP_C_AFF_PX,       /* affine BG                          */
  VP_C_BMP_CALL, VP_C_BMP_PX,       /* bitmap-mode BG                     */
  VP_C_OBJ_CALL,                    /* render_scanline_objs() calls       */
  VP_C_OBJ_SPR,                     /* sprites actually drawn (per line)  */
  VP_C_OBJ_AFF,                     /* ...of which affine                 */
  VP_C_MERGE_PX,                    /* merge_blend() pixels               */
  VP_C_BRIGHT_PX,                   /* merge_brightness() pixels          */
  VP_C_FILL_PX,                     /* fill_line_background() pixels      */
  VP_C_BDROP_PX,                    /* render_backdrop() pixels           */
  VP_C_PIXCOPY,                     /* obj-window PIXCOPY sprite passes   */
  /* --- overdraw (level 3): is the 4x really wasted work? --------------- */
  VP_C_BGWR,      /* BG pixels actually STORED (opaque, or base backdrop)  */
  VP_C_BGOCC,     /* ...of which a later layer overwrote: WASTED stores    */
  VP_C_BGL0, VP_C_BGL1, VP_C_BGL2, VP_C_BGL3,   /* stores, per BG layer    */
  VP_C_TROW,      /* full 8-px 4bpp tile rows drawn                         */
  VP_C_TROWFULL,  /* ...of which every one of the 8 pixels is OPAQUE, i.e.  */
                  /* the tile row below it is entirely wasted work          */
  VP_RD,                            /* clock reads (prices the probe)     */
  VP_COUNT
};

#ifdef __cplusplus
extern "C" {
#endif

/* The frontend installs a microsecond clock and reads the accumulators.
 * Both are no-ops in a build without VIDEO_PROF (see video.cc). */
void video_prof_set_clock(unsigned (*clock_us)(void));
/* Copy the current frame's accumulators into `out` (VP_COUNT entries) and
 * zero them.  Returns the number of entries written (0 if not compiled in). */
int  video_prof_read_reset(unsigned *out, int n);
/* Short names for the buckets, VP_COUNT entries, for log formatting. */
const char *video_prof_name(int idx);

#ifdef __cplusplus
}
#endif

#if VIDEO_PROF

#ifdef __cplusplus
extern "C" {
#endif
extern unsigned video_prof_acc[VP_COUNT];
extern unsigned (*video_prof_clk)(void);
extern unsigned video_prof_depth[VP_T_FILL + 1];
extern unsigned video_prof_t0[VP_T_FILL + 1];
#ifdef __cplusplus
}
#endif

#define VP_CNT(b, n)   do { video_prof_acc[b] += (unsigned)(n); } while (0)

/* Levels, so the probe prices itself instead of being asserted (ADR-0032):
 *   VIDEO_PROF=1  counters + the update_scanline() total only  (~322 reads/f)
 *   VIDEO_PROF=2  + the per-path brackets                      (~1820 reads/f)
 * ttot at level 1 vs level 2 IS the inner probe's cost, measured. */
#if VIDEO_PROF >= 2
#define VP_ENTER2(b)   VP_ENTER(b)
#define VP_LEAVE2(b)   VP_LEAVE(b)
#else
#define VP_ENTER2(b)   do { } while (0)
#define VP_LEAVE2(b)   do { } while (0)
#endif

/* Level 3: per-STORE coverage tracking, to separate "rendered 4x" from
 * "rendered 4x and three of them were thrown away".  Layers paint back to
 * front, so a store to a pixel that has already been stored this scanline
 * means the EARLIER store was wasted.  This is deliberately inside the hot
 * loop and therefore level 3 only — it distorts timing badly and the counts
 * are what matter, not the clock. */
#if VIDEO_PROF >= 3
#ifdef __cplusplus
extern "C" {
#endif
extern u16 *video_prof_cover_base;   /* scanline the BG layers paint into */
extern u8   video_prof_cover[240];
extern u32  video_prof_layer;        /* BG layer currently painting, 0-3   */
#ifdef __cplusplus
}
#endif
#define VP_COVER(p)                                                          \
  do {                                                                       \
    if (video_prof_cover_base) {                                             \
      size_t _i = (size_t)((u16*)(p) - video_prof_cover_base);               \
      if (_i < 240) {                                                        \
        if (video_prof_cover[_i]) video_prof_acc[VP_C_BGOCC]++;              \
        video_prof_cover[_i] = 1;                                            \
        video_prof_acc[VP_C_BGWR]++;                                         \
        video_prof_acc[VP_C_BGL0 + (video_prof_layer & 3)]++;                \
      }                                                                      \
    }                                                                        \
  } while (0)
#define VP_COVER_LINE(base)                                                  \
  do { video_prof_cover_base = (base);                                       \
       memset(video_prof_cover, 0, sizeof(video_prof_cover)); } while (0)
#define VP_LAYER(l)    do { video_prof_layer = (l); } while (0)
#else
#define VP_COVER(p)         do { } while (0)
#define VP_COVER_LINE(base) do { } while (0)
#define VP_LAYER(l)         do { } while (0)
#endif

/* Only the outermost bracket for a bucket is timed; inner ones just track
 * depth.  A bucket therefore reports its inclusive cost exactly once. */
#define VP_ENTER(b)                                                          \
  do {                                                                       \
    if (video_prof_clk && video_prof_depth[b]++ == 0) {                      \
      video_prof_t0[b] = video_prof_clk();                                   \
      video_prof_acc[VP_RD]++;                                               \
    }                                                                        \
  } while (0)

#define VP_LEAVE(b)                                                          \
  do {                                                                       \
    if (video_prof_clk && --video_prof_depth[b] == 0) {                      \
      video_prof_acc[b] += video_prof_clk() - video_prof_t0[b];              \
      video_prof_acc[VP_RD]++;                                               \
    }                                                                        \
  } while (0)

#else  /* !VIDEO_PROF — everything vanishes */

#define VP_CNT(b, n)   do { } while (0)
#define VP_ENTER(b)    do { } while (0)
#define VP_LEAVE(b)    do { } while (0)
#define VP_ENTER2(b)   do { } while (0)
#define VP_LEAVE2(b)   do { } while (0)
#define VP_COVER(p)         do { } while (0)
#define VP_COVER_LINE(base) do { } while (0)
#define VP_LAYER(l)         do { } while (0)

#endif

#endif /* VIDEO_PROF_H */

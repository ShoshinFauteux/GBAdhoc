/* me_mbox.h — the ONE shared contract between the main CPU (Allegrex/SC)
 * and the Media Engine (ME).  Included by BOTH the kernel PRX (ME side)
 * and the user EBOOT (host side); nothing else may define this layout.
 *
 * ADR-0080 (ME bring-up).  Design rules, each load-bearing:
 *
 *  - The mailbox lives in MAIN RAM, allocated by the app, 64-byte aligned,
 *    and is ALWAYS accessed through the 0x40000000 uncached alias by both
 *    processors.  The SC and ME have no cache coherency; uncached access is
 *    the only protocol that needs no cache choreography.  64-byte alignment
 *    keeps it alone on its cache line for any accidental cached touch.
 *  - PSP user-space pointers are physical-identity on this hardware (no
 *    TLB), so the SAME numeric pointer is valid on the SC (kuseg cached),
 *    the SC uncached alias (|0x40000000), and the ME (kuseg, same rules).
 *    That is what makes a shared struct possible at all.
 *  - Command protocol is strictly sequence-numbered SPSC: the host bumps
 *    cmd_seq after filling cmd/args; the ME executes and copies cmd_seq to
 *    done_seq.  done_seq == cmd_seq means idle.  The host NEVER waits on
 *    the ME inside the frame loop — it checks and drops (the whole point).
 *  - heartbeat increments every dispatcher iteration, so "ME wedged" and
 *    "ME idle" are distinguishable from the host (ADR-0058's rule applied
 *    to a coprocessor).
 *  - No function pointers cross the boundary.  Every job the ME can run is
 *    compiled INTO the kernel PRX and named by ME_CMD_*.  User .text never
 *    executes on the ME: no I-cache aliasing questions, no GP concerns, no
 *    accidental syscall/VFPU use in ME context.
 */
#ifndef ME_MBOX_H
#define ME_MBOX_H

#define ME_MBOX_MAGIC   0x4D454F4Bu   /* "MEOK": ME writes this at boot */
/* v3: the renderer's work area is handed over in the render desc (below), the
 * ME reports a refused render, and ME_CMD_SYNC exists.  The bump is what keeps
 * a v2 EBOOT from driving a v3 module: its 28-byte desc has no work fields, so
 * the module would read whatever followed it in memory.  A mismatched pair
 * fails the boot handshake and the app renders on the main CPU. */
#define ME_MBOX_VERSION 3

/* Commands.  Keep the set tiny; every entry is code resident in the PRX. */
#define ME_CMD_NOP        0
/* Copy arg0 -> arg1, arg2 bytes.  arg3 flags: bit0 = ME reads src via its
 * D-cache (with invalidate before / nothing after, since src is SC-written
 * RAM); bit1 = ME writes dst via its D-cache (with writeback-invalidate
 * after).  Flags 0 = fully uncached both sides — the always-correct mode. */
#define ME_CMD_COPY       1
/* The Stage-0 microbenchmark: run arg2-byte copies arg3 times from arg0 to
 * arg1 in each access mode, reporting per-mode iteration counts... kept
 * simpler: one mode per invocation (host varies flags), result = number of
 * sceKernelGetSystemTimeLow-free "work units" is impossible on the ME (no
 * syscalls), so the HOST times the round trip; result carries a checksum
 * of the last copy so the work cannot be optimised away or faked. */
#define ME_CMD_BENCH_COPY 2
/* Park the dispatcher (suspend/teardown).  ME acks then spins on magic;
 * the host clears magic to release it back into the loop after resume. */
#define ME_CMD_PARK       3
/* Pitched copy — the video staging job.  arg0=src, arg1=dst,
 * arg2 = rows<<16 | row_bytes, arg4=src_pitch, arg5=dst_pitch,
 * arg3 = flags as ME_CMD_COPY.  row_bytes must be a multiple of 32
 * (the GBA frame's 480-byte rows are 15*32). */
#define ME_CMD_COPY_PITCH 4
/* Render a GBA frame on the ME (the renderer offload).  arg0 = physical/uncached
 * pointer to an me_render_desc (below); the ME reads the frame's input snapshot
 * through it and writes the finished 240x160 RGB565 frame to desc->out.  The
 * input set is the one proven bit-exact on the desktop (RENDER_REPLAY, ~81k
 * frames, 0 mismatch): vram + oam + palette_converted + a per-line io_registers
 * log + the affine seed + the OAM-updated flag.  result = a checksum of the
 * output frame (host verifies, same discipline as ME_CMD_COPY). */
#define ME_CMD_RENDER     5
/* v3.  Write back and invalidate the ME's whole D-cache.  The renderer's work
 * area (below) is written through that cache, so up to 16 KiB of it sits in
 * dirty lines the ME writes back whenever it next needs the line.  The host
 * posts this, and waits for it, before handing HEAP-backed work memory to
 * free(): free() writes its list pointers into the block, and a late
 * writeback from the ME would land on top of them. */
#define ME_CMD_SYNC       6

/* ---- THE RENDERER'S WORK AREA (v3) ---------------------------------------
 *
 * Three buffers the renderer needs and nobody else ever looks at: its mirror
 * of VRAM, the per-line OBJ lists order_obj() builds, and the frame it draws
 * into before the pitched write-out.  They were static arrays in the module
 * -- 278 KB of the kernel partition, on top of 130 KB of code -- and a module
 * that needs 403 KB contiguous does not load on a PSP-1000 whose kernel
 * memory is partly taken (PSPLink: largest free block 283 KB; CFW plugins in
 * the field).  The load fails with 0x800200D9 and the app silently draws on
 * the main CPU.
 *
 * So the HOST provides them, from the same volatile pool its capture and
 * stage buffers come from, and names them in every render desc.  The module
 * keeps no pointer between renders: it takes the base from the desc of the
 * command it is executing and refuses the render if there is none.
 *
 * WHO TOUCHES IT.  The ME, through its D-cache, and nothing else.  The host
 * holds the address only to pass it on: it never reads or writes the area, so
 * there is no coherency protocol to get wrong.  That is also why the base and
 * every size are whole 64-byte cache lines -- no line of the work area can be
 * shared with memory the main CPU does write. */
#define ME_WORK_VRAM_BYTES (1024u * 96u)          /*  98,304: u8 vram[96K]    */
#define ME_WORK_OBJ_BYTES  (5u * 160u * 128u)     /* 102,400: OBJ lists       */
#define ME_WORK_OUT_BYTES  77312u                 /* u16[240*161] = 77,280,
                                                   * rounded up to a line     */
#define ME_WORK_VRAM_OFF   0u
#define ME_WORK_OBJ_OFF    (ME_WORK_VRAM_OFF + ME_WORK_VRAM_BYTES)
#define ME_WORK_OUT_OFF    (ME_WORK_OBJ_OFF + ME_WORK_OBJ_BYTES)
#define ME_WORK_BYTES      (ME_WORK_OUT_OFF + ME_WORK_OUT_BYTES)   /* 278,016 */

/* v2 render contract (ME_CMD_RENDER, arg0 = desc pointer).  The LIVE pipeline:
 *
 *   1. Host (at end of emulated frame N): writes back its caches for VRAM /
 *      OAM / converted-palette / the capture log, fills this desc, posts.
 *   2. ME: snapshots vram/oam/palette from the LIVE core arrays into its own
 *      PRX-resident copies (cached reads, invalidate first), then writes
 *      mb->input_seq = the command's seq.  FROM THAT MOMENT the core arrays
 *      are free — the host resumes emulating frame N+1 in parallel.
 *   3. ME: renders 160 lines from the me_capture_frame (the per-line LCD
 *      register log + affine seed + OAM flag — the input set proven bit-exact
 *      by RENDER_REPLAY and ME_CAP_VALIDATE), then writes the finished frame
 *      to `out` as pitched rows (out_pitch pixels per row) and sets done_seq.
 *   4. Host (end of frame N+1): sees done, presents `out`, posts N+1.
 *
 * `capture` points at a me_capture_frame (video.h); this header deliberately
 * does not depend on that type — the address is enough for the C sides. */
typedef struct me_render_desc
{
   volatile unsigned int vram;         /* live core vram (u8[96KB])         */
   volatile unsigned int oam;          /* live core oam_ram (u16[512])      */
   volatile unsigned int palette;      /* live palette_ram_converted        */
   volatile unsigned int capture;      /* me_capture_frame for this frame   */
   volatile unsigned int out;          /* pitched RGB565 output buffer      */
   volatile unsigned int out_pitch;    /* output stride in PIXELS (e.g 512) */
   /* DIRTY-PAGE MAP: u8[96], one byte per 1 KiB VRAM page, 1 = clean.
    * The ME copies only the pages marked dirty. A consecutive-frame compare
    * measured 0.1-0.5 of 96 pages changing per frame, so this turns a 96 KB
    * copy into ~2 KB. Zero means "copy this page".
    * If 0, the ME copies ALL of VRAM -- used on the first frame and after any
    * teardown/resume, when the ME's eDRAM mirror cannot be trusted. */
   volatile unsigned int vram_clean;
   /* v3: the work area (above).  `work` is its base, 64-byte aligned;
    * `work_bytes` is what the host really reserved there, and the ME refuses
    * anything smaller than ME_WORK_BYTES rather than trust a constant the two
    * binaries were each compiled with.
    *
    * `work_gen` changes every time the host (re)builds the renderer.  The
    * area's CONTENTS do not survive that: the pool is handed back across a
    * sleep and the heap block is freed around a wireless bring-up, while the
    * module -- and its idea of what the area holds -- can stay loaded
    * throughout.  A base or generation the ME has not seen means "start from
    * nothing": it copies all of VRAM whatever vram_clean says and clears its
    * OBJ lists, exactly the state of a module that was just loaded. */
   volatile unsigned int work;
   volatile unsigned int work_bytes;
   volatile unsigned int work_gen;
   /* PRESENTATION COLOUR (video.h).  APPENDED, and guarded by its own magic
    * rather than a version bump, so either half can meet the other:
    *   - a module older than these fields never reads them: the host's
    *     palette and the bitmap pixels reach the screen untransformed, as
    *     before (the picture is native; nothing else changes);
    *   - this module driven by an older EBOOT reads past that EBOOT's desc
    *     and finds no ME_COLOR_MAGIC there: colour stays native.
    * color_lut is the EBOOT's own gba_cc_lut (.rodata, 64 KiB, never
    * written): the engine reads it through its D-cache and invalidates it
    * once per address it has not seen.  Nothing new is allocated anywhere. */
   volatile unsigned int color_magic;  /* ME_COLOR_MAGIC: the two below hold */
   volatile unsigned int color_mode;   /* VIDEO_COLOR_* (0 = native)        */
   volatile unsigned int color_lut;    /* gba_cc_lut, for VIDEO_COLOR_GBA   */
} me_render_desc;
#define ME_COLOR_MAGIC 0x434F4C31u     /* "COL1" */
#define ME_COLOR_LUT_BYTES (32768u * 2u)

typedef struct me_mbox
{
   /* --- written by ME, read by host ------------------------------------ */
   volatile unsigned int magic;       /* ME_MBOX_MAGIC once the ME is up   */
   volatile unsigned int heartbeat;   /* ++ every dispatcher iteration     */
   volatile unsigned int done_seq;    /* last completed command            */
   volatile unsigned int result;      /* command-specific (checksum etc.)  */
   /* --- written by host, read by ME ------------------------------------ */
   volatile unsigned int cmd_seq;     /* bump AFTER filling cmd/args       */
   volatile unsigned int cmd;
   volatile unsigned int arg0;        /* src  (user/physical pointer)      */
   volatile unsigned int arg1;        /* dst                               */
   volatile unsigned int arg2;        /* len                               */
   volatile unsigned int arg3;        /* flags                             */
   /* --- extended args (ME_CMD_COPY_PITCH) ------------------------------- */
   volatile unsigned int arg4;        /* src_pitch                         */
   volatile unsigned int arg5;        /* dst_pitch                         */
   /* --- diagnostics ----------------------------------------------------- */
   volatile unsigned int me_faults;   /* reserved (exception counting)     */
   volatile unsigned int version;     /* ME_MBOX_VERSION, written by ME    */
   /* --- v2: input-consumed handshake (ME_CMD_RENDER) --------------------- */
   volatile unsigned int input_seq;   /* ME sets = seq once it has copied
                                       * the live core arrays; the host may
                                       * then resume emulation while the ME
                                       * renders in parallel               */
   /* --- v3: refused render ---------------------------------------------- */
   volatile unsigned int refused_seq; /* ME sets = seq of a render it did not
                                       * draw because the desc named no usable
                                       * work area.  The command still
                                       * completes (input_seq, done_seq), so
                                       * the host never hangs on it; it reads
                                       * this at retire and falls back to the
                                       * main CPU.  0 = never (seq starts at 1) */
} me_mbox;

/* Uncached alias helper — valid for user main-RAM pointers on both cores. */
#define ME_UNCACHED(p) ((void *)(0x40000000u | (unsigned int)(p)))

#endif /* ME_MBOX_H */

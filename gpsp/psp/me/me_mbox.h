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
#define ME_MBOX_VERSION 2             /* v2: input_seq + render desc v2 */

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
} me_render_desc;

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
} me_mbox;

/* Uncached alias helper — valid for user main-RAM pointers on both cores. */
#define ME_UNCACHED(p) ((void *)(0x40000000u | (unsigned int)(p)))

#endif /* ME_MBOX_H */

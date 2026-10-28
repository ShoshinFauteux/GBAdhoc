/* fe_gblink.h -- a Game Boy / Game Boy Color link session between two
 * consoles, without a live serial link.
 *
 * Each console runs BOTH players' Game Boys (gbcore/gbcore_dual.h): slot 0
 * is the host's cartridge, slot 1 the guest's, on both consoles, with the
 * serial ports cabled together in memory.  Only buttons cross the network,
 * in lockstep with a fixed input delay: a player's buttons for his machine's
 * frame f+D are sampled when that machine starts frame f and sent at once,
 * and a machine whose buttons for its next frame have not arrived waits
 * (a stall) -- late packets cost time, never correctness.
 *
 * Session, over one reliable ordered channel (netdrv via fe_np_gb_send):
 *
 *   HELLO   both: protocol, role, cartridge (size, SHA-1, title, console,
 *           palette), battery image size.
 *   CONFIG  host: input delay, RTC seed (the host's clock, for both
 *           machines), hash interval, the pair's scanline batching.
 *   NEED    each: whether it has the partner's cartridge (by SHA-1, via the
 *           platform's library lookup).  If not, the partner streams it
 *           (ROM chunks, memory only -- never written to storage) and the
 *           receiver checks the SHA-1 before using it: ROM_OK / ROM_BAD.
 *   SAVE    each streams its battery image, clock trailer re-based to the
 *           RTC seed so both consoles load the same clock.
 *   STATE   each streams its running game's save state (a LIVE link; the
 *           machine then starts from it, the battery image supplying the
 *           clock), so linking does not restart either game.
 *   READY   both -> both machines power on from identical inputs.
 *   INPUT   each: its player's buttons, by that machine's frame number.
 *   HASH    each: gbcore sync hash of both machines every hash_interval
 *           frames of slot 0; a mismatch is a desync: stop, never save.
 *   END     either asks; the pair settles on a slot-0 frame both can
 *           still reach (later while either cartridge's RAM is still
 *           changing, so a save is never cut in half), each captures its
 *           own machine's battery image and the final hash there, and
 *           FINAL hashes are compared.  Only if they are equal does each
 *           console commit its OWN side's save.
 *   ABORT   either, with a reason.
 *
 * Single-threaded: every call from the thread that pumps the network.
 * Pure C over gbcore_dual and a platform vtable, so the whole protocol is
 * host-tested (tools/tests/test_gblink.c) with a simulated network.
 */
#ifndef FE_GBLINK_H
#define FE_GBLINK_H

#include <stddef.h>
#include <stdint.h>

#include "fe_console.h"
#include "../gbcore/gbcore_dual.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 6: the sync hash is word-based (gbcore_tgbdual.c sync_bytes); a console
 * on 5 would report every periodic hash as a desync, so it is refused at
 * HELLO instead. */
#define FE_GBLINK_PROTO 6
/* The session's first message, for a platform that must hold one arriving
 * before its own session exists (fe_host.c). */
#define FE_GBLINK_HELLO_TYPE 0x01
#define FE_GBLINK_HELLO_LEN  67u

/* Bulk lane (cartridge and save transfers).  A datagram:
 *   'G' 'B' 'L' 'K' | kind (0 cartridge, 1 save) | u32 chunk index |
 *   u32 CRC-32 of the data | data
 * sent unreliably outside the ordered channel; the receiver acknowledges
 * over the ordered channel (M_BULK_ACK: first missing chunk + a bitmap of
 * the next 256), and the sender resends only what is missing.  Why: on the
 * PSP the ordered channel's cumulative acknowledgement stalls a whole
 * window behind every lost datagram, and hardware run hw1 lost ~3 %: a
 * 2 MiB cartridge took 372 s (PPSSPP with 3 % injected loss: the same
 * collapse).  Datagrams are also ~7x larger than the ordered channel's
 * 144-byte limit, and the PSP radio's cost is per datagram. */
#define FE_GBLINK_BULK_MAGIC "GBLK"
#define FE_GBLINK_BULK_HDR  13u

typedef struct fe_gblink fe_gblink;

/* The machines a session runs (docs/GBA-LINK-MIRROR.md section 7).
 * fe_gblink_local.machines NULL = the GB/GBC pair (gbcore_dual), exactly as
 * before.  Otherwise this vtable: today fe_gbadual's GBA mirror (two gpSP
 * instances cabled in memory).  The protocol, the transfers, the lockstep
 * and the end agreement are the same code for both.
 *
 * Differences the session honours for a vtable pair:
 *   - the own cartridge is SHA-1'd by streaming, never held in memory (the
 *     running machine already has it; a 32 MiB GBA cart would not fit twice);
 *   - the partner's cartridge must be the same cartridge (SHA-1): no library
 *     lookup and no transfer yet (two-ROM pairs are a later milestone);
 *   - no clock trailer re-base on the battery image;
 *   - CONFIG's batch field carries the pair's own scheduling quantum
 *     (GBA: the lookahead in scanlines, 114 = half a frame);
 *   - the final hash is taken when SLOT 0 completes the agreed frame (both
 *     consoles compute it at the same point), but each console captures its
 *     own machine's battery and state when THAT machine completes the agreed
 *     frame: a GBA machine can only be serialized at its own frame boundary. */
typedef struct fe_gblink_machines
{
   void *user;
   uint8_t console;              /* FE_CONSOLE_* sent in HELLO; peer must match */
   uint32_t header_off;          /* the 0x1C header bytes HELLO carries */
   uint32_t rom_min, rom_max;    /* cartridge size bounds */
   uint32_t save_max;            /* battery image bound */
   /* Start both machines (slot 0 = the host's) from their states and battery
    * images.  frame_hook is called whenever either completes a frame (frame
    * = frames that machine has completed).  clock_seed: the session's clock
    * (CONFIG's, the host's), for anything that would read a console's own.
    * 0 = running. */
   int (*start)(void *user, int local_slot, unsigned batch, int64_t clock_seed,
                const uint8_t *const state[2], const size_t state_n[2],
                const uint8_t *const save[2], const size_t save_n[2],
                void (*frame_hook)(void *hu, int slot, uint64_t frame),
                void *hook_user);
   /* Until the local machine completes a frame (1), a machine needs buttons
    * input() cannot give yet (0: resumable), or the pair failed (-1). */
   int (*advance)(void *user,
                  int (*input)(void *iu, int slot, uint64_t frame,
                               uint16_t *buttons),
                  void *input_user);
   /* Both machines' guest state; equal on both consoles at equal points. */
   uint64_t (*sync_hash)(void *user);
   /* A cheap fingerprint of `slot`'s battery RAM (end deferral). */
   uint32_t (*cart_crc)(void *user, int slot);
   size_t (*save_size)(void *user, int slot);
   int (*save_read)(void *user, int slot, void *buf, size_t n);  /* 0 = ok */
   size_t (*state_size)(void *user, int slot);
   int (*state_save)(void *user, int slot, void *buf, size_t n); /* 0 = ok */
   /* The session is over: leave link mode (the local machine continues). */
   void (*stop)(void *user);
} fe_gblink_machines;

typedef struct fe_gblink_platform
{
   void *user;
   /* Reliable, ordered, to the one peer.  0 on success. */
   int (*send)(void *user, const void *buf, size_t len);
   /* Largest payload send() accepts, and how many more it will take now
    * without piling up (bulk transfers pace themselves on it). */
   size_t max_payload;
   int (*send_room)(void *user);
   /* Bulk pacing: at most this many cartridge/save chunks per step (per
    * display frame); 0 = no limit beyond send_room.  A burst larger than
    * the receiver drains in a frame is dropped by its transport ring and
    * costs a retransmission timeout, so a steady rate is faster. */
   unsigned bulk_per_step;
   /* The local library: fill `path` with a cartridge of exactly `size`
    * bytes whose header matches `header` (0x134..0x14F).  The session
    * verifies the SHA-1 itself.  Return 1 if a candidate was found. */
   int (*find_rom)(void *user, uint32_t size, const uint8_t header[0x1C],
                   char *path, size_t cap);
   /* Write this console's own battery image at the end (only after the
    * final hashes agreed).  0 on success. */
   int (*commit_save)(void *user, const void *image, size_t size);
   /* The local machine's picture and sound. */
   void (*video)(void *user, const gbcore_video_frame_t *frame);
   void (*audio)(void *user, const int16_t *samples, size_t frames);
   /* Bulk lane (optional; both consoles must offer it or the ordered
    * channel carries the transfers): send one datagram of at most
    * bulk_max bytes to the partner, unreliably; 0 = sent or dropped.
    * Received ones arrive through fe_gblink_receive_bulk. */
   int (*bulk_send)(void *user, const void *buf, size_t len);
   size_t bulk_max;
   /* Datagrams per step: 0 = adapt to the share resent (start 4, double
    * while under 6 %, then +1; -25 % over 12 %, -50 % over 25 %; at most
    * bulk_rate_max, default 16). */
   unsigned bulk_rate;
   unsigned bulk_rate_max;
   /* Inputs, also as one unreliable bulk-lane datagram a frame carrying
    * this player's last `input_copies` frames of buttons (0 = off, at most
    * 16), beside the ordered channel's copy.  One lost datagram is then
    * covered by the next frame's instead of stalling the game until the
    * ordered channel resends it (hw1: 11-40 stalls a run on the PSP-1000,
    * up to 7 frames long, at ~3 % loss). */
   unsigned input_copies;
   /* Stall absorption (optional; docs/GB-LINK.md section 14).  When a step
    * is held up by the partner's buttons, the session calls wait() until
    * they are here or wait() returns 0: it must deliver what the network
    * has (pump it once, receive() runs inside) and say whether this display
    * frame still has time for the rest of its work.  `waited_us` is how
    * long this step has waited so far.  NULL = a late input returns STALL
    * at once and the whole display frame is lost, however close it was.
    * Only WHEN a step returns changes: the machines advance on the inputs
    * alone, so absorbing a wait never changes what they compute. */
   int (*wait)(void *user, uint32_t waited_us);
   /* Resend requests (needs the bulk lane): a display frame lost waiting for
    * the partner's buttons asks the partner for them on the bulk lane
    * (BK_NACK, fe_gblink.c) instead of waiting for the ordered channel's
    * retransmission timeout behind a lost datagram.  0 = off. */
   int nack;
} fe_gblink_platform;

typedef struct fe_gblink_local
{
   int is_host;
   const char *rom_path;        /* this console's cartridge */
   fe_console_t console;
   unsigned palette;
   const void *save;            /* its battery image (may be NULL/0) */
   size_t save_size;
   /* A LIVE link: this console's running game as a gbcore save state,
    * taken at a frame boundary when the player chose Host/Join.  Its
    * machine starts from it (on both consoles) instead of powering on from
    * the battery image, which still travels: the state does not carry the
    * cartridge clock, the battery image's trailer does.  NULL/0 = power on
    * from the battery image. */
   const void *state;
   size_t state_size;
   /* Step on slot 0's frame boundary on BOTH consoles (the guest's own
    * machine then finishes its frame one scanline into the next step).
    * Stepping on the local machine instead, the guest's pair needs the
    * host's buttons for frame f+1 one scanline before its own frame f ends
    * -- a frame earlier, relative to its own timeline, than the host needs
    * the guest's: the guest had one frame less slack (hw2: the join
    * stalled 134-885 times a run, the host 0-26).  Local only: it changes
    * no machine state, only when step() returns. */
   int pace_slot0;
   int64_t wallclock_now;       /* seconds since 1970, this console */
   unsigned audio_rate;
   /* Host only (the guest takes the host's): input delay in frames,
    * hash interval in slot-0 frames. */
   unsigned input_delay;
   unsigned hash_interval;
   /* Host only: gbdual_config_t.batch_lines for both consoles (0/1 =
    * strict line-by-line alternation); a vtable pair's quantum. */
   unsigned batch_lines;
   /* The session's switches (docs/GBA-LINK-MIRROR.md 26, 29).
    *
    * STRICT bits change what the machines compute (bit 0: TIMER_LAZY): both
    * consoles must run the same set, and a guest whose own differs refuses
    * the session before it starts.
    *
    * NEGOTIABLE bits change only how the session talks (bit 1: "Fast link",
    * two frames of buttons per packet).  THE HOST DECIDES: the guest's own
    * value is ignored and it runs what the host selected -- if both builds
    * can.  How each side learns that, without breaking a build that knows
    * none of this (3.5.0, whose unknown-message rule is "abort"):
    *   - the host's HELLO role byte carries FE_GBLINK_HOSTCAP_NEGOTIATE
    *     (an old guest only tests that byte for non-zero);
    *   - a guest that sees it answers M_CAPS (what it can do) and holds its
    *     READY until the host's M_CORECFG (what is in force) arrives;
    *   - a host that never got M_CAPS (an old guest) sends nothing new and
    *     clears its negotiable bits; a guest whose host did not advertise
    *     (an old host) sends nothing new and clears its own.
    * So an old build on either side means the plain session. */
   uint32_t core_mask;
#define FE_GBLINK_MASK_TIMER_LAZY  0x1u   /* strict: guest-visible core switch */
#define FE_GBLINK_MASK_INPUT_BATCH 0x2u   /* negotiable: "Fast link", M_INPUTS */
/* negotiable: GB pair stepping, gbdual_config_t.batch_armed.  The machines
 * compute the same frames either way (exact), but the session's HASH
 * points read slot 1 mid-frame, so both consoles must step alike: the host
 * decides, and an older build on either side means it is off. */
#define FE_GBLINK_MASK_ARMED_BATCH 0x4u
#define FE_GBLINK_MASK_NEGOTIABLE  (FE_GBLINK_MASK_INPUT_BATCH | \
                                    FE_GBLINK_MASK_ARMED_BATCH)
#define FE_GBLINK_HOSTCAP_NEGOTIATE 0x02u /* in the host's HELLO role byte     */
   /* NULL = the GB/GBC pair; else the machines above (console must match
    * machines->console). */
   const fe_gblink_machines *machines;
   /* Harness A/B only, both consoles alike: the 3.1 beta's byte-wise sync
    * hash and cartridge-RAM CRC (gbcore_set_sync_hash_legacy). */
   int hash_legacy;
   /* A vtable pair only, both consoles alike: identify the own cartridge by
    * its size and 64 evenly spread 4 KiB slices instead of the whole file
    * (fe_gblink.c SAMPLE_N; docs/GBA-LINK-UI.md).  0 = the streamed SHA-1. */
   int rom_sampled;
} fe_gblink_local;

enum fe_gblink_state
{
   FE_GBLINK_SETUP = 0,         /* handshake, transfers */
   FE_GBLINK_RUNNING,
   FE_GBLINK_ENDING,            /* agreed end frame not reached / hashes */
   FE_GBLINK_DONE,              /* hashes agreed, own save committed */
   FE_GBLINK_FAILED             /* see fe_gblink_error(); nothing written */
};

enum fe_gblink_error
{
   FE_GBLINK_OK = 0,
   FE_GBLINK_E_PROTO,           /* incompatible peer / malformed message */
   FE_GBLINK_E_ROM_MISSING,     /* partner cartridge unavailable */
   FE_GBLINK_E_ROM_BAD,         /* received cartridge failed its SHA-1 */
   FE_GBLINK_E_MEMORY,
   FE_GBLINK_E_START,           /* the machines would not power on */
   FE_GBLINK_E_DESYNC,          /* hashes disagreed */
   FE_GBLINK_E_PEER_LOST,
   FE_GBLINK_E_PEER_ABORT,
   FE_GBLINK_E_COMMIT,          /* agreed, but the save write failed */
   FE_GBLINK_E_LOCAL,           /* we ended it before it started */
   FE_GBLINK_E_NO_PEER          /* nobody answered within the connect time */
};

/* Step results */
enum { FE_GBLINK_IDLE = 0, FE_GBLINK_FRAME = 1, FE_GBLINK_STALL = 2 };

fe_gblink *fe_gblink_create(const fe_gblink_platform *p,
                            const fe_gblink_local *l);
void fe_gblink_destroy(fe_gblink *s);

/* Network events. */
void fe_gblink_receive(fe_gblink *s, const void *buf, size_t len);
/* A bulk-lane datagram (anything else is counted and ignored). */
void fe_gblink_receive_bulk(fe_gblink *s, const void *buf, size_t len);
void fe_gblink_peer(fe_gblink *s, int connected);

/* Once per display frame.  During SETUP it drives the handshake and the
 * transfers and returns IDLE.  Once RUNNING it advances the machines until
 * the local one completes a frame (FRAME) or a partner input is late
 * (STALL); `local_buttons` (GBCORE_BUTTON_*) are this player's buttons, used
 * only when the local machine starts a new frame, i.e. after a FRAME. */
int fe_gblink_step(fe_gblink *s, uint16_t local_buttons);

/* Ask for a clean end (the menu's Disconnect).  During SETUP it aborts. */
void fe_gblink_request_end(fe_gblink *s);

int fe_gblink_state(const fe_gblink *s);
/* "Fast link" as this session runs it: 1 on, 0 off (final once the session
 * is RUNNING).  *source (optional): where the value came from. */
enum { FE_GBLINK_FL_OWN = 0,      /* this console is the host: its setting   */
       FE_GBLINK_FL_HOST,         /* this console is the guest: the host's   */
       FE_GBLINK_FL_OLD_PEER };   /* off: the other build cannot do it       */
int fe_gblink_fast_link(const fe_gblink *s, int *source);
int fe_gblink_error(const fe_gblink *s);
const char *fe_gblink_error_text(int error);
/* Two short lines for the connecting screen / status chip. */
void fe_gblink_status(const fe_gblink *s, char *line1, size_t cap1,
                      char *line2, size_t cap2);
/* The same two lines' progress, per mille (-1 = the line has no bar). */
void fe_gblink_status_progress(const fe_gblink *s, int *p1, int *p2);

/* The connecting screen's facts, for a frontend that words them itself
 * (docs/GBA-LINK-UI.md, the LINKING screen).  Read-only. */
enum
{
   FE_GBLINK_PH_CHECK = 0,      /* identifying the own cartridge */
   FE_GBLINK_PH_WAIT_PEER,      /* no partner on the channel yet */
   FE_GBLINK_PH_HELLO,          /* handshake: HELLO / CONFIG / NEED */
   FE_GBLINK_PH_ROM_RX,         /* receiving the partner's cartridge */
   FE_GBLINK_PH_ROM_TX,         /* sending ours */
   FE_GBLINK_PH_VERIFY,         /* checking a library candidate */
   FE_GBLINK_PH_SHARE,          /* saves and states crossing */
   FE_GBLINK_PH_START,          /* everything here, READY exchange */
   FE_GBLINK_PH_RUNNING,
   FE_GBLINK_PH_ENDING,
   FE_GBLINK_PH_DONE,
   FE_GBLINK_PH_FAILED
};
typedef struct fe_gblink_progress
{
   int state, error, phase;
   int peer_reason;             /* the partner's ABORT reason, -1 = none */
   int ran;                     /* the machines started (RUNNING reached) */
   int peer_up;
   uint32_t done, total;        /* the phase's amount (0/0 = none) */
   char peer_title[17];         /* the partner cartridge's title, or "" */
   int have_peer_header;
   uint8_t peer_header[0x1C];   /* its raw header (GBA: 0xA0.., GB: 0x134..) */
} fe_gblink_progress;
void fe_gblink_get_progress(const fe_gblink *s, fe_gblink_progress *out);

/* Introspection (telemetry, tests). */
typedef struct fe_gblink_stats
{
   int local_slot;
   unsigned input_delay;
   uint64_t frames[GBDUAL_SLOTS];
   uint32_t stalls;               /* step() calls that returned STALL */
   uint32_t stall_streak_max;     /* longest run of consecutive stalls */
   uint32_t stall_episodes;       /* runs of stalls (a stutter each) */
   uint32_t hashes_sent, hashes_matched;
   uint64_t last_hash;
   uint32_t last_hash_frame;
   uint32_t rom_tx_bytes, rom_rx_bytes, rom_total;
   uint64_t rom_rx_first_us, rom_rx_last_us;   /* 0 = not timed */
   uint32_t save_tx_bytes, save_rx_bytes;
   uint32_t end_frame;
   int rom_sent, rom_received;    /* a cartridge crossed the link */
   /* bulk lane: chunk data bytes (0 = ordered channel), datagrams sent,
    * of which resends, datagrams received (incl. duplicates), rate now */
   uint32_t bulk_chunk, bulk_sent, bulk_resent, bulk_rx, bulk_dup, bulk_bad;
   unsigned bulk_rate;
   /* inputs: taken first from the redundant datagram / from the ordered
    * channel */
   uint32_t inputs_fast, inputs_ordered;
   /* resend requests sent / answered for the partner / inputs taken from
    * the partner's answers */
   uint32_t nacks, resends, inputs_resent;
} fe_gblink_stats;
void fe_gblink_get_stats(const fe_gblink *s, fe_gblink_stats *out);

/* Per-window telemetry (docs/GB-LINK.md section 14): what a RUNNING step
 * cost and why it was late, accumulated since the last take (which resets
 * it).  Times need fe_gblink_set_clock (0 without one). */
typedef struct fe_gblink_window
{
   uint32_t steps;                /* step() calls while running */
   uint32_t frames;               /* ...that returned FRAME */
   uint32_t stalls;               /* ...that returned STALL (frame lost) */
   uint32_t stall_episodes;       /* runs of STALLs starting this window */
   uint32_t streak_max;           /* longest run of STALLs this window */
   uint32_t absorbed;             /* steps that waited and still made it */
   uint32_t absorb_us, absorb_us_max;  /* time those waits took */
   uint32_t wait_us;              /* all waiting, absorbed or not */
   /* the machines (both cores, gbdual_advance) vs the whole step */
   uint32_t adv_us, adv_us_max;
   uint32_t step_us, step_us_max;
   /* session housekeeping inside the step: sync hashes, cartridge-RAM checks */
   uint32_t hash_n, hash_us, hash_us_max;
   uint32_t crc_n, crc_us, crc_us_max;
   /* the partner's buttons in hand, beyond the frame needed, each time its
    * machine started a frame on time: min/sum over lead_n (late = stalls) */
   int32_t lead_min;
   uint32_t lead_n, lead_sum;
   uint32_t nacks, resends;       /* resend requests sent / answered */
   unsigned delay;                /* this console's input delay now */
} fe_gblink_window;
void fe_gblink_take_window(fe_gblink *s, fe_gblink_window *out);

/* After DONE: this console's own machine as it stood at the agreed end
 * frame -- its battery image (clock trailer stamped with the SESSION clock)
 * and its save state -- so the single machine can continue exactly there.
 * NULL/0 before DONE. */
const void *fe_gblink_final_save(const fe_gblink *s, size_t *size);
const void *fe_gblink_final_state(const fe_gblink *s, size_t *size);

/* The running pair (NULL before RUNNING): scripts read the local
 * machine's RAM through it. */
gbdual_t *fe_gblink_dual(const fe_gblink *s);
int fe_gblink_local_slot(const fe_gblink *s);

/* Clock for transfer timing (optional; microseconds). */
void fe_gblink_set_clock(fe_gblink *s, uint64_t (*now_us)(void));

#ifdef __cplusplus
}
#endif
#endif

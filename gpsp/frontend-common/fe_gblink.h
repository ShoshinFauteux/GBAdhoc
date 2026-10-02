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

#define FE_GBLINK_PROTO 5
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
    * strict line-by-line alternation). */
   unsigned batch_lines;
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
int fe_gblink_error(const fe_gblink *s);
const char *fe_gblink_error_text(int error);
/* Two short lines for the connecting screen / status chip. */
void fe_gblink_status(const fe_gblink *s, char *line1, size_t cap1,
                      char *line2, size_t cap2);
/* The same two lines' progress, per mille (-1 = the line has no bar). */
void fe_gblink_status_progress(const fe_gblink *s, int *p1, int *p2);

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
} fe_gblink_stats;
void fe_gblink_get_stats(const fe_gblink *s, fe_gblink_stats *out);

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

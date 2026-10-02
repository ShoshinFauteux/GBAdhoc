/* sesssim -- two consoles' GB link sessions in one process, over a simulated
 * network, each driven by its player's autopilot script.
 *
 * Each "console" is a complete stack: fe_gblink (the session protocol)
 * over gbcore_dual (both players' Game Boys) over two TGB Dual instances.
 * tools/gblink/build.sh links the stack twice -- console H uses instances
 * A/B, console G the renamed copies C/D, with fe_gblink/gbdual renamed for
 * G -- so the two consoles share no state, exactly as two PSPs.  The
 * network is a queue per direction with a latency and a jitter in frames
 * (reliable and ordered, as netdrv delivers); time advances one display
 * frame per tick on both consoles.
 *
 *   sesssim rom_h=.. rom_g=.. [sav_h=..] [sav_g=..] [ap_h=..] [ap_g=..]
 *           [delay=N] [lat=FRAMES] [jitter=FRAMES] [seed=N] [max=TICKS]
 *           [rtc=SECONDS] [out_h=..] [out_g=..] [hashlog=FILE]
 *           [lib_h=DIR] [lib_g=DIR] [desync_at=FRAME] [payload=BYTES]
 *           [quit_after=FRAMES] [room=SLOTS] [skew=SECONDS]
 *           [corrupt_rom=N]   (the Nth ROM chunk arrives with a flipped bit)
 *           [st_h=STATE] [st_g=STATE]  a LIVE link: each console links from
 *                         its running game, this gbcore save state
 *           (a script's `disconnect` asks its console to end the session)
 *           [desync_addr=ADDR] (the byte desync_at flips; default $C100)
 *           [bulk=BYTES]  bulk-lane datagram size (0 = transfers on the
 *                         ordered channel); [bulk_loss=PCT] that share of
 *                         bulk datagrams lost, [bulk_rate=N] datagrams per
 *                         frame (0 = adaptive), [bulk_cap=N] the network
 *                         carries at most N bulk datagrams per frame and
 *                         direction (the rest are lost, as a full radio),
 *                         [corrupt_bulk=N] the Nth bulk datagram damaged
 *           [arq_loss=PCT] [arq_rto=FRAMES]  that share of ordered-channel
 *                         messages arrives arq_rto frames late (a resend),
 *                         holding everything behind it
 *
 * lib_*: the console's library for the partner's cartridge ("<dir>/any
 * file of the right size and header"); absent means the cartridge must be
 * transferred.  desync_at: at that slot-0 frame, flip one byte of the
 * guest console's copy of slot 0's WRAM (the session must detect it and
 * write no save).  quit_after: frames after both scripts finish before the
 * host asks to end.  Prints EVT lines; exit 0 when both sessions end DONE.
 */
#include "../../frontend-common/fe_gblink.h"
#include "../../frontend-common/fe_autopilot.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the guest console's renamed stack ------------------------------- */
typedef struct fe_gblink fe_gblink2;
fe_gblink *fe_gblink2_create(const fe_gblink_platform *, const fe_gblink_local *);
void fe_gblink2_destroy(fe_gblink *);
void fe_gblink2_receive(fe_gblink *, const void *, size_t);
void fe_gblink2_receive_bulk(fe_gblink *, const void *, size_t);
void fe_gblink2_peer(fe_gblink *, int);
int fe_gblink2_step(fe_gblink *, uint16_t);
void fe_gblink2_request_end(fe_gblink *);
int fe_gblink2_state(const fe_gblink *);
int fe_gblink2_error(const fe_gblink *);
void fe_gblink2_get_stats(const fe_gblink *, fe_gblink_stats *);
gbdual_t *fe_gblink2_dual(const fe_gblink *);
const gbcore_api_t *gbdual2_api(int);
gbcore_t *gbdual2_core(const gbdual_t *, int);
uint64_t gbdual2_sync_hash(gbdual_t *);
void fe_gblink2_set_clock(fe_gblink *, uint64_t (*)(void));
int fe_autopilot_b_load(const char *);
void fe_autopilot_b_frame(void);
int fe_autopilot_b_status(void);
int fe_autopilot_disconnect_pending(void);
int fe_autopilot_b_disconnect_pending(void);

typedef struct stack
{
   fe_gblink *(*create)(const fe_gblink_platform *, const fe_gblink_local *);
   void (*destroy)(fe_gblink *);
   void (*receive)(fe_gblink *, const void *, size_t);
   void (*receive_bulk)(fe_gblink *, const void *, size_t);
   void (*peer)(fe_gblink *, int);
   int (*step)(fe_gblink *, uint16_t);
   void (*request_end)(fe_gblink *);
   int (*state)(const fe_gblink *);
   int (*error)(const fe_gblink *);
   void (*stats)(const fe_gblink *, fe_gblink_stats *);
   gbdual_t *(*dual)(const fe_gblink *);
   const gbcore_api_t *(*api)(int);
   gbcore_t *(*core)(const gbdual_t *, int);
   uint64_t (*hash)(gbdual_t *);
} stack_t;

static const stack_t STACK[2] = {
   { fe_gblink_create, fe_gblink_destroy, fe_gblink_receive,
     fe_gblink_receive_bulk, fe_gblink_peer,
     fe_gblink_step, fe_gblink_request_end, fe_gblink_state, fe_gblink_error,
     fe_gblink_get_stats, fe_gblink_dual, gbdual_api, gbdual_core,
     gbdual_sync_hash },
   { fe_gblink2_create, fe_gblink2_destroy, fe_gblink2_receive,
     fe_gblink2_receive_bulk, fe_gblink2_peer, fe_gblink2_step, fe_gblink2_request_end,
     fe_gblink2_state, fe_gblink2_error, fe_gblink2_get_stats,
     fe_gblink2_dual, gbdual2_api, gbdual2_core, gbdual2_sync_hash },
};

/* ---- logging ---------------------------------------------------------- */
static void vlog(const char *tag, const char *fmt, va_list ap)
{
   printf("EVT ");
   vprintf(fmt, ap);
   printf(" console=%s\n", tag);
}
void fe_evt(const char *fmt, ...) { va_list a; va_start(a, fmt); vlog("H", fmt, a); va_end(a); }
void sim_evt2(const char *fmt, ...) { va_list a; va_start(a, fmt); vlog("G", fmt, a); va_end(a); }
void ap_b_evt(const char *fmt, ...) { va_list a; va_start(a, fmt); vlog("G", fmt, a); va_end(a); }
static void sim(const char *fmt, ...) { va_list a; va_start(a, fmt); vlog("sim", fmt, a); va_end(a); }
void fe_log(const char *fmt, ...) { va_list a; va_start(a, fmt); printf("LOG "); vprintf(fmt, a); printf("\n"); va_end(a); }
unsigned long long fe_evt_now_us(void) { return 0; }

/* ---- the network ------------------------------------------------------ */
typedef struct msg { uint64_t due; size_t len; uint8_t *data; struct msg *next; } msg_t;
static msg_t *q_head[2], *q_tail[2];    /* [to console] */
static uint64_t tick;
static unsigned lat, jit;
/* the ordered channel as an ARQ over a lossy radio: a lost datagram is
 * resent after `arq_rto` frames, and everything behind it waits */
static unsigned arq_loss, arq_rto = 4;
static uint32_t rng = 12345;
static size_t payload = 144;
static unsigned room_slots = 256;
static unsigned in_flight[2];           /* from console */
static uint64_t bytes_sent[2];

static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

static int corrupt_rom;                 /* flip a byte of one ROM chunk */

static int net_send(int from, const void *buf, size_t len)
{
   msg_t *m;
   uint64_t due;
   if (len > payload)
   {
      sim("oversize from=%d len=%u", from, (unsigned)len);
      exit(3);
   }
   m = (msg_t *)calloc(1, sizeof(*m));
   m->data = (uint8_t *)malloc(len);
   memcpy(m->data, buf, len);
   m->len = len;
   if (corrupt_rom && len > 64 && m->data[0] == 0x04 && --corrupt_rom == 0)
   {
      m->data[40] ^= 0x10;               /* damaged in flight */
      sim("rom_chunk_corrupted from=%d", from);
   }
   due = tick + lat + (jit ? rnd() % (jit + 1) : 0);
   if (arq_loss && rnd() % 100 < arq_loss)
      due += arq_rto;
   /* ordered: never before the one queued ahead of it */
   if (q_tail[!from] && q_tail[!from]->due > due)
      due = q_tail[!from]->due;
   m->due = due;
   if (q_tail[!from]) q_tail[!from]->next = m; else q_head[!from] = m;
   q_tail[!from] = m;
   in_flight[from]++;
   bytes_sent[from] += len;
   return 0;
}

static fe_gblink *sess[2];
static uint64_t sim_us(void) { return tick * 16667ull; }

/* The bulk lane: unordered, lossy, same latency and jitter. */
static msg_t *bq[2];                     /* [to console], unsorted */
static unsigned bulk_loss, bulk_cap;
static int corrupt_bulk;                 /* flip a byte of the Nth datagram */
static uint64_t bulk_tick[2];
static unsigned bulk_this_tick[2];
static uint64_t bulk_sent_n[2], bulk_lost_n[2];

static int bulk_send(int from, const void *buf, size_t len)
{
   msg_t *m;
   if (bulk_tick[from] != tick)
   {
      bulk_tick[from] = tick;
      bulk_this_tick[from] = 0;
   }
   bulk_sent_n[from]++;
   bytes_sent[from] += len;
   if ((bulk_cap && ++bulk_this_tick[from] > bulk_cap) ||
       (bulk_loss && rnd() % 100 < bulk_loss))
   {
      bulk_lost_n[from]++;
      return 0;
   }
   m = (msg_t *)calloc(1, sizeof(*m));
   m->data = (uint8_t *)malloc(len);
   memcpy(m->data, buf, len);
   m->len = len;
   if (corrupt_bulk && len > 64 && --corrupt_bulk == 0)
   {
      m->data[len - 7] ^= 0x10;          /* damaged in flight */
      sim("bulk_datagram_corrupted from=%d", from);
   }
   m->due = tick + lat + (jit ? rnd() % (jit + 1) : 0);
   m->next = bq[!from];
   bq[!from] = m;
   return 0;
}

static void bulk_deliver(int to)
{
   msg_t **pp = &bq[to];
   while (*pp)
   {
      msg_t *m = *pp;
      if (m->due <= tick)
      {
         *pp = m->next;
         if (sess[to])
            STACK[to].receive_bulk(sess[to], m->data, m->len);
         free(m->data);
         free(m);
      }
      else
         pp = &m->next;
   }
}

static void net_deliver(int to)
{
   while (q_head[to] && q_head[to]->due <= tick)
   {
      msg_t *m = q_head[to];
      q_head[to] = m->next;
      if (!q_head[to]) q_tail[to] = NULL;
      in_flight[!to]--;
      if (sess[to])
         STACK[to].receive(sess[to], m->data, m->len);
      free(m->data);
      free(m);
   }
}

/* ---- platform callbacks ----------------------------------------------- */
typedef struct console { int id; const char *lib; const char *out; int committed; } console_t;
static console_t con[2];

static int p_send(void *u, const void *b, size_t n) { return net_send(((console_t *)u)->id, b, n); }
static int p_bulk(void *u, const void *b, size_t n) { return bulk_send(((console_t *)u)->id, b, n); }
static int p_room(void *u) { int id = ((console_t *)u)->id; return (int)room_slots - (int)in_flight[id]; }

static int p_find(void *u, uint32_t size, const uint8_t header[0x1C], char *path, size_t cap)
{
   console_t *c = (console_t *)u;
   DIR *d;
   struct dirent *e;
   if (!c->lib || !(d = opendir(c->lib)))
      return 0;
   while ((e = readdir(d)))
   {
      char p[1024];
      FILE *f;
      long n;
      uint8_t h[0x1C];
      snprintf(p, sizeof(p), "%s/%s", c->lib, e->d_name);
      if (!(f = fopen(p, "rb"))) continue;
      fseek(f, 0, SEEK_END);
      n = ftell(f);
      if (n == (long)size && !fseek(f, 0x134, SEEK_SET) &&
          fread(h, 1, 0x1C, f) == 0x1C && !memcmp(h, header, 0x1C))
      {
         fclose(f);
         closedir(d);
         snprintf(path, cap, "%s", p);
         return 1;
      }
      fclose(f);
   }
   closedir(d);
   return 0;
}

static int p_commit(void *u, const void *img, size_t n)
{
   console_t *c = (console_t *)u;
   FILE *f;
   c->committed = 1;
   if (!c->out) return 0;
   f = fopen(c->out, "wb");
   if (!f || fwrite(img, 1, n, f) != n) { if (f) fclose(f); return -1; }
   fclose(f);
   return 0;
}

/* ---- autopilot hooks: each player's script reads its own machine ------ */
static uint32_t injected[2];
static int mem_read(int id, uint32_t a, void *o, unsigned n)
{
   gbdual_t *d = sess[id] ? STACK[id].dual(sess[id]) : NULL;
   if (!d || a > 0xFFFF) return -1;
   return STACK[id].api(id)->peek(STACK[id].core(d, id), (uint16_t)a, o, n);
}
int fe_host_mem_read(uint32_t a, void *o, unsigned n) { return mem_read(0, a, o, n); }
int ap_b_host_mem_read(uint32_t a, void *o, unsigned n) { return mem_read(1, a, o, n); }
void fe_host_input_inject(uint32_t m) { injected[0] = m; }
void ap_b_host_input_inject(uint32_t m) { injected[1] = m; }
uint32_t fe_host_sram_crc_now(void) { return 0; }
uint32_t ap_b_host_sram_crc_now(void) { return 0; }

static uint16_t gb_buttons(uint32_t m)
{
   uint16_t o = 0;
   if (m & (1u << 0)) o |= GBCORE_BUTTON_B;
   if (m & (1u << 2)) o |= GBCORE_BUTTON_SELECT;
   if (m & (1u << 3)) o |= GBCORE_BUTTON_START;
   if (m & (1u << 4)) o |= GBCORE_BUTTON_UP;
   if (m & (1u << 5)) o |= GBCORE_BUTTON_DOWN;
   if (m & (1u << 6)) o |= GBCORE_BUTTON_LEFT;
   if (m & (1u << 7)) o |= GBCORE_BUTTON_RIGHT;
   if (m & (1u << 8)) o |= GBCORE_BUTTON_A;
   return o;
}

/* ---- main ------------------------------------------------------------- */
static const char *arg(int argc, char **argv, const char *k, const char *def)
{
   size_t n = strlen(k);
   int i;
   for (i = 1; i < argc; i++)
      if (!strncmp(argv[i], k, n) && argv[i][n] == '=')
         return argv[i] + n + 1;
   return def;
}

static uint8_t *slurp(const char *p, size_t *n)
{
   FILE *f = fopen(p, "rb");
   uint8_t *b;
   long l;
   if (!f) return NULL;
   fseek(f, 0, SEEK_END); l = ftell(f); fseek(f, 0, SEEK_SET);
   b = (uint8_t *)malloc(l > 0 ? (size_t)l : 1);
   if (l > 0 && fread(b, 1, (size_t)l, f) != (size_t)l) { free(b); b = NULL; }
   fclose(f);
   *n = l > 0 ? (size_t)l : 0;
   return b;
}

static fe_console_t console_of(const char *rom)
{
   const char *d = strrchr(rom, '.');
   return d && (d[3] == 'c' || d[3] == 'C') ? FE_CONSOLE_GBC : FE_CONSOLE_GB;
}

int main(int argc, char **argv)
{
   const char *rom[2] = { arg(argc, argv, "rom_h", NULL), arg(argc, argv, "rom_g", NULL) };
   const char *sav[2] = { arg(argc, argv, "sav_h", NULL), arg(argc, argv, "sav_g", NULL) };
   const char *ap[2] = { arg(argc, argv, "ap_h", NULL), arg(argc, argv, "ap_g", NULL) };
   const char *stf[2] = { arg(argc, argv, "st_h", NULL), arg(argc, argv, "st_g", NULL) };
   const char *hashlog_path = arg(argc, argv, "hashlog", NULL);
   uint64_t max = strtoull(arg(argc, argv, "max", "40000"), NULL, 0);
   uint64_t desync_at = strtoull(arg(argc, argv, "desync_at", "0"), NULL, 0);
   uint16_t desync_addr = (uint16_t)strtoul(arg(argc, argv, "desync_addr", "0xC100"), NULL, 0);
   unsigned quit_after = (unsigned)atoi(arg(argc, argv, "quit_after", "120"));
   int have_script[2] = { 0, 0 }, done_at[2] = { -1, -1 };
   int started[2] = { 0, 0 };
   uint64_t scripts_done_tick = 0, last_hashed = 0;
   int end_asked = 0, desynced = 0, i, rc = 0;
   FILE *hashlog = NULL;
   uint64_t stall_ticks[2] = { 0, 0 };

   if (!rom[0] || !rom[1]) { fprintf(stderr, "usage: see sesssim.c\n"); return 2; }
   lat = (unsigned)atoi(arg(argc, argv, "lat", "2"));
   jit = (unsigned)atoi(arg(argc, argv, "jitter", "0"));
   rng = (uint32_t)strtoul(arg(argc, argv, "seed", "12345"), NULL, 0);
   payload = (size_t)atoi(arg(argc, argv, "payload", "144"));
   room_slots = (unsigned)atoi(arg(argc, argv, "room", "256"));
   corrupt_rom = atoi(arg(argc, argv, "corrupt_rom", "0"));
   bulk_loss = (unsigned)atoi(arg(argc, argv, "bulk_loss", "0"));
   bulk_cap = (unsigned)atoi(arg(argc, argv, "bulk_cap", "0"));
   arq_loss = (unsigned)atoi(arg(argc, argv, "arq_loss", "0"));
   arq_rto = (unsigned)atoi(arg(argc, argv, "arq_rto", "4"));
   corrupt_bulk = atoi(arg(argc, argv, "corrupt_bulk", "0"));
   con[0].id = 0; con[1].id = 1;
   con[0].lib = arg(argc, argv, "lib_h", NULL);
   con[1].lib = arg(argc, argv, "lib_g", NULL);
   con[0].out = arg(argc, argv, "out_h", NULL);
   con[1].out = arg(argc, argv, "out_g", NULL);
   if (hashlog_path) hashlog = fopen(hashlog_path, "w");

   for (i = 0; i < 2; i++)
   {
      fe_gblink_platform p;
      fe_gblink_local l;
      size_t sn = 0;
      uint8_t *sd = sav[i] ? slurp(sav[i], &sn) : NULL;
      size_t stn = 0;
      uint8_t *std_ = stf[i] ? slurp(stf[i], &stn) : NULL;
      memset(&p, 0, sizeof(p));
      memset(&l, 0, sizeof(l));
      p.user = &con[i];
      p.send = p_send;
      p.max_payload = payload;
      p.send_room = p_room;
      p.find_rom = p_find;
      p.commit_save = p_commit;
      p.bulk_max = (size_t)atoi(arg(argc, argv, "bulk", "0"));
      p.bulk_send = p.bulk_max ? p_bulk : NULL;
      p.bulk_rate = (unsigned)atoi(arg(argc, argv, "bulk_rate", "0"));
      p.input_copies = (unsigned)atoi(arg(argc, argv, "input_copies", "0"));
      l.is_host = i == 0;
      l.rom_path = rom[i];
      l.console = console_of(rom[i]);
      l.palette = 0;
      l.save = sd;
      l.save_size = sn;
      l.state = std_;
      l.state_size = stn;
      /* skew: the guest console's clock is this many seconds ahead. */
      l.wallclock_now = strtoll(arg(argc, argv, "rtc", "1790500000"), NULL, 0) +
                        (i ? strtoll(arg(argc, argv, "skew", "0"), NULL, 0) : 0);
      l.audio_rate = 32768;
      l.input_delay = (unsigned)atoi(arg(argc, argv, "delay", "4"));
      l.hash_interval = 60;
      l.batch_lines = (unsigned)atoi(arg(argc, argv, "batch", "0"));
      l.pace_slot0 = atoi(arg(argc, argv, "pace0", "0"));
      sess[i] = STACK[i].create(&p, &l);
      free(sd);
      free(std_);
      if (!sess[i]) { fprintf(stderr, "create %d failed\n", i); return 2; }
      (i ? fe_gblink2_set_clock : fe_gblink_set_clock)(sess[i], sim_us);
   }
   if (ap[0]) { if (fe_autopilot_load(ap[0])) return 2; have_script[0] = 1; }
   if (ap[1]) { if (fe_autopilot_b_load(ap[1])) return 2; have_script[1] = 1; }
   STACK[0].peer(sess[0], 1);
   STACK[1].peer(sess[1], 1);
   sim("start delay=%s lat=%u jitter=%u payload=%u", arg(argc, argv, "delay", "4"),
       lat, jit, (unsigned)payload);

   for (tick = 0; tick < max; tick++)
   {
      int st[2];
      for (i = 0; i < 2; i++)
      {
         int r;
         net_deliver(i);
         bulk_deliver(i);
         st[i] = STACK[i].state(sess[i]);
         if (st[i] == FE_GBLINK_RUNNING || st[i] == FE_GBLINK_ENDING)
         {
            /* The script's first look is at power-on, before frame 0,
             * exactly as linkplay runs it (and as a PSP does). */
            if (!started[i] && have_script[i])
            {
               if (i == 0) fe_autopilot_frame(); else fe_autopilot_b_frame();
            }
            started[i] = 1;
            r = STACK[i].step(sess[i], gb_buttons(injected[i]));
            if (r == FE_GBLINK_FRAME && have_script[i])
            {
               if (i == 0) fe_autopilot_frame(); else fe_autopilot_b_frame();
            }
            if (r == FE_GBLINK_STALL) stall_ticks[i]++;
         }
         else
            STACK[i].step(sess[i], 0);
      }
      /* the desync injection: the guest console's copy of slot 0 */
      if (desync_at && !desynced)
      {
         gbdual_t *d = STACK[1].dual(sess[1]);
         fe_gblink_stats s;
         STACK[1].stats(sess[1], &s);
         if (d && s.frames[0] >= desync_at)
         {
            uint8_t v;
            STACK[1].api(0)->peek(STACK[1].core(d, 0), desync_addr, &v, 1);
            v ^= 0x5A;
            STACK[1].api(0)->poke(STACK[1].core(d, 0), desync_addr, &v, 1);
            desynced = 1;
            sim("desync_injected slot0_frame=%llu", (unsigned long long)s.frames[0]);
         }
      }
      if (hashlog)
      {
         fe_gblink_stats s;
         STACK[0].stats(sess[0], &s);
         if (s.last_hash_frame && s.last_hash_frame != last_hashed)
         {
            fprintf(hashlog, "%u %016llx\n", s.last_hash_frame,
                    (unsigned long long)s.last_hash);
            last_hashed = s.last_hash_frame;
         }
      }
      for (i = 0; i < 2; i++)
         if (have_script[i] &&
             (i ? fe_autopilot_b_disconnect_pending()
                : fe_autopilot_disconnect_pending()))
         {
            sim("script_disconnect console=%d", i);
            STACK[i].request_end(sess[i]);
            end_asked = 1;
         }
      for (i = 0; i < 2; i++)
         if (have_script[i] && done_at[i] < 0 &&
             (i ? fe_autopilot_b_status() : fe_autopilot_status()) != 0)
         {
            done_at[i] = (int)tick;
            sim("script_done console=%d status=%d", i,
                i ? fe_autopilot_b_status() : fe_autopilot_status());
         }
      if (!end_asked && (!have_script[0] || done_at[0] >= 0) &&
          (!have_script[1] || done_at[1] >= 0) &&
          STACK[0].state(sess[0]) == FE_GBLINK_RUNNING)
      {
         if (!scripts_done_tick) scripts_done_tick = tick;
         if (tick - scripts_done_tick >= quit_after)
         {
            STACK[0].request_end(sess[0]);
            end_asked = 1;
         }
      }
      st[0] = STACK[0].state(sess[0]);
      st[1] = STACK[1].state(sess[1]);
      if ((st[0] == FE_GBLINK_DONE || st[0] == FE_GBLINK_FAILED) &&
          (st[1] == FE_GBLINK_DONE || st[1] == FE_GBLINK_FAILED) &&
          !q_head[0] && !q_head[1] && !bq[0] && !bq[1])
         break;
   }
   for (i = 0; i < 2; i++)
   {
      fe_gblink_stats s;
      STACK[i].stats(sess[i], &s);
      sim("result console=%s state=%d error=%d committed=%d frames=%llu/%llu "
          "stall_ticks=%llu stalls=%u streak_max=%u hashes=%u/%u end=%u "
          "rom_tx=%u rom_rx=%u rom_rx_us=%llu bytes_sent=%llu "
          "bulk_chunk=%u bulk_sent=%u resent=%u rx=%u dup=%u bad=%u rate=%u "
          "net_bulk_lost=%llu/%llu episodes=%u inputs_fast=%u ordered=%u",
          i ? "G" : "H", STACK[i].state(sess[i]), STACK[i].error(sess[i]),
          con[i].committed, (unsigned long long)s.frames[0],
          (unsigned long long)s.frames[1], (unsigned long long)stall_ticks[i],
          s.stalls, s.stall_streak_max, s.hashes_matched, s.hashes_sent,
          s.end_frame, s.rom_tx_bytes, s.rom_rx_bytes,
          (unsigned long long)(s.rom_rx_last_us - s.rom_rx_first_us),
          (unsigned long long)bytes_sent[i], s.bulk_chunk, s.bulk_sent,
          s.bulk_resent, s.bulk_rx, s.bulk_dup, s.bulk_bad, s.bulk_rate,
          (unsigned long long)bulk_lost_n[i],
          (unsigned long long)bulk_sent_n[i], s.stall_episodes,
          s.inputs_fast, s.inputs_ordered);
      if (STACK[i].state(sess[i]) != FE_GBLINK_DONE) rc = 1;
   }
   sim("ticks=%llu", (unsigned long long)tick);
   if (hashlog) fclose(hashlog);
   STACK[0].destroy(sess[0]);
   STACK[1].destroy(sess[1]);
   return rc;
}

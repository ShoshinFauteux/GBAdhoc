/* mgift_cart.c — the emulated Pokémon distribution cart.  See mgift_cart.h for
 * what this is, why it lives on this side, and why it cannot regress trading.
 *
 * Every constant and layout below is cited to the source it came from: the pret
 * decompilation (public community C) for the game's side, and gpsp/rfu.c for the
 * adapter emulation's side.  Nothing here is guessed -- where a value was not
 * verifiable it is marked and defaulted conservatively.
 */

#include "mgift_cart.h"

#include <stdio.h>
#include <string.h>

#include "fe_evt.h"
#include "fe_host.h"
#include "libretro.h"

/* ---- L2: rfu.c's peer frames (rfu.c:930) -------------------------------- */

#define RFU1_MAGIC          0x52465531u   /* 'RFU1' */
#define RFU1_HDR            12

#define NET_RFU_BROADCAST   0x00
#define NET_RFU_CONNECT_REQ 0x01
#define NET_RFU_CONNECT_ACK 0x02
#define NET_RFU_DISCONNECT  0x04
#define NET_RFU_HOST_SEND   0x05
#define NET_RFU_CLIENT_SEND 0x06
#define NET_RFU_CLIENT_ACK  0x07

/* ---- L2.5: the game's command/block protocol (link_rfu_2.c) -------------- */

#define CMD_LENGTH          8     /* u16 words in a command array */
#define COMM_SLOT_LENGTH    14    /* bytes actually on the wire (link_rfu.h:28) */
#define FRAG_BYTES          12    /* data bytes per SEND_BLOCK fragment: the
                                   * receiver copies cmd[1..6] only, so the
                                   * sender's 7th word is ignored padding
                                   * (link_rfu_2.c:1161) */
#define MAX_FRAGS           32    /* receivedFlags is a 32-bit mask ->
                                   * 32 * 12 = 384 bytes per block, max */

#define RFUCMD_MASK              0xFF00
#define RFUCMD_SEND_PLAYER_IDS   0x7700
#define RFUCMD_SEND_BLOCK_INIT   0x8800
#define RFUCMD_SEND_BLOCK        0x8900
#define RFUCMD_SEND_BLOCK_REQ    0xA100
#define RFUCMD_READY_CLOSE_LINK  0x5F00
#define RFUCMD_READY_STANDBY     0x6600
#define RFUCMD_DISCONNECT        0xED00

/* ---- L2 gate: what makes the game see a DISTRIBUTOR --------------------- */

/* THE ONE VALUE THAT MATTERS FOR DISCOVERY.
 *
 * FireRed's Mystery Gift screen calls Rfu_GetWonderDistributorPlayerData
 * (link_rfu_3.c:917), whose entire test is
 *
 *     if (gRfuLinkStatus->partner[idx].serialNo == RFU_SERIAL_WONDER_DISTRIBUTOR)
 *
 * An ordinary Pokémon game broadcasts RFU_SERIAL_GAME (0x0002) and is offered
 * as a trade/battle partner; 0x7F7D is what says "I am a distribution cart"
 * (link_rfu.h:24-25).  Get this wrong and the menu simply never lists us. */
#define RFU_SERIAL_WONDER_DISTRIBUTOR 0x7F7D

/* DEVICE IDS ARE 16 BITS, and that is load-bearing rather than cosmetic.
 *
 * When the player picks us, the game asks rfu.c to connect to a device id and
 * rfu.c matches it against the one we broadcast:
 *
 *     u16 reqid = rfu_buf[0] & 0xffff;                      // rfu.c:1229
 *     if (rfu_peer_bcst[i].device_id == reqid)              // rfu.c:1231
 *
 * A broadcast id wider than 16 bits can therefore never match what comes back,
 * and the connection fails with no error anywhere -- the menu simply does
 * nothing.  rfu.c's own new_devid() returns a u16 for exactly this reason.
 *
 * OUR_DEVID is the cart advertising itself; CLIENT_DEVID is what we hand the
 * game in CONNECT_ACK as its own id, which a real host generates fresh.  They
 * must differ. */
#define MGC_OUR_DEVID    0x1001u
#define MGC_CLIENT_DEVID 0x1002u

#define RFU_GAME_NAME_LENGTH 13       /* librfu.h:102 */
#define RFU_USER_NAME_LENGTH 8        /* librfu.h:103 */

/* ACTIVITY_WONDER_CARD, constants/union_room.h:46. */
#define ACTIVITY_WONDER_CARD 21

/* ---- L3: MysteryGiftLink (mystery_gift_link.c) --------------------------- */

#define MGL_HDR_SIZE   6      /* struct SendRecvHeader { u16 ident, crc, size } */
#define MGL_CHUNK      252
#define MGL_BUF        0x400

#define MG_LINKID_CLIENT_SCRIPT 16
#define MG_LINKID_GAME_DATA     17
#define MG_LINKID_RESPONSE      19
#define MG_LINKID_READY_END     20
#define MG_LINKID_CARD          22
#define MG_LINKID_RAM_SCRIPT    25

/* ---- L4: client script opcodes (mystery_gift_client.h) ------------------- */

#define CLI_RETURN            1
#define CLI_RECV              2
#define CLI_SEND_LOADED       3
#define CLI_COPY_RECV         4
#define CLI_LOAD_GAME_DATA    8
#define CLI_SAVE_CARD        10
#define CLI_ASK_TOSS         13
#define CLI_LOAD_TOSS_RESPONSE 14
#define CLI_SAVE_RAM_SCRIPT  17
#define CLI_SEND_READY_END   20

#define CLI_MSG_CARD_RECEIVED 2
#define CLI_MSG_HAD_CARD      5
#define CLI_MSG_CANT_ACCEPT  10
#define CLI_MSG_COMM_CANCELED 9

#define CARD_BYTES 332

/* ---- state --------------------------------------------------------------- */

/* Server phases.  This is gMysteryGiftServerScript_SendWonderCard flattened:
 * the full script VM buys nothing when there is exactly one script to run, and
 * a flat phase list is far easier to read against the decomp. */
enum
{
   SV_IDLE = 0,
   SV_SEND_GAMEDATA_SCRIPT,   /* send sClientScript_SendGameData */
   SV_RECV_GAMEDATA,          /* RECV MG_LINKID_GAME_DATA        */
   SV_SEND_SAVECARD_SCRIPT,   /* send sClientScript_SaveCard     */
   SV_SEND_CARD,              /* send the 332-byte card          */
   SV_SEND_RAMSCRIPT,         /* send the game's redemption script */
   SV_RECV_READY_END,         /* RECV MG_LINKID_READY_END        */
   SV_DONE,
   SV_REFUSED,
   SV_SEND_TOSS_SCRIPT,
   SV_RECV_TOSS,
   SV_SEND_RESULT,
   SV_RECV_RESULT,
   SV_FAILED
};

static struct
{
   int  active;
   int  linked;               /* the game connected to us */
   int  devid;                /* device id we handed the client */
   int connect_pending;
   uint32_t ni_ack;
   int name_end;
   unsigned join_stage;
   unsigned join_wait;
   unsigned exchange;
   unsigned exchange_wait;
   uint8_t echo[COMM_SLOT_LENGTH];
   uint8_t parent_cmd[COMM_SLOT_LENGTH];
   uint8_t control[COMM_SLOT_LENGTH];
   int control_pending;
   int standby_seen;
   int uni_seen;

   unsigned frame;            /* our own frame counter */
   unsigned last_bcast;

   uint8_t  card[CARD_BYTES];
   int      have_card;

   /* L2.5 send side */
   uint8_t  blk_tx[MGL_BUF];
   unsigned blk_tx_len;
   unsigned blk_tx_frags;
   unsigned blk_tx_next;
   int      blk_tx_init_left;  /* INIT repeats before fragments (>2, l_r_2:1372) */
   int      blk_tx_busy;

   /* L2.5 receive side */
   uint8_t  blk_rx[MGL_BUF];
   unsigned blk_rx_frags;
   uint32_t blk_rx_flags;
   int      blk_rx_busy;
   int      blk_rx_done;

   /* L3 */
   int      svr;               /* SV_* */
   int      mgl_stage;         /* 0 = header, 1 = body */
   unsigned mgl_body_off;
   unsigned recv_ident, recv_size, recv_off;
   uint16_t recv_crc;
   int recv_header;
   uint8_t recv_body[MGL_BUF];
   uint8_t ram_script[995]; /* sizeof(RamScriptData.script), FRLG and Emerald */
   unsigned result;
   unsigned last_progress;
   unsigned last_rx;

   mgift_cart_stats st;
   char     status[48];
} C;

static const struct retro_netpacket_callback *g_cb;
static mgift_parcel P;
static const uint8_t *selected_script;
static unsigned selected_script_size;
static void cart_receive_llsf(const uint8_t *data, unsigned len);
static void mgl_expect(unsigned ident);

/* ---- byte helpers ------------------------------------------------------- */

static void pk32be(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
   p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static uint32_t up32be(const uint8_t *p)
{
   return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
          ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static void pk16le(uint8_t *p, uint16_t v)
{
   p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

static uint16_t up16le(const uint8_t *p)
{
   return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t up32le(const uint8_t *p)
{
   return up16le(p) | ((uint32_t)up16le(p + 2) << 16);
}

/* CalcCRC16WithTable, pokefirered/src/util.c:250.  The byte mixing is unusual --
 * the table is indexed by the LOW byte after xoring the data in, and the
 * previous high byte is xored over the result -- so it is written out here
 * rather than reached for from a CRC library. */
static uint16_t mgl_crc16(const uint8_t *data, unsigned len)
{
   static uint16_t tab[256];
   static int ready;
   uint16_t crc = 0x1121;
   unsigned i, j;

   if (!ready)
   {
      for (i = 0; i < 256; i++)
      {
         uint16_t c = (uint16_t)i;
         for (j = 0; j < 8; j++)
            c = (c & 1) ? (uint16_t)((c >> 1) ^ 0x8408) : (uint16_t)(c >> 1);
         tab[i] = c;
      }
      ready = 1;
   }
   for (i = 0; i < len; i++)
   {
      uint16_t hi = (uint16_t)(crc >> 8);
      crc = (uint16_t)(crc ^ data[i]);
      crc = (uint16_t)(hi ^ tab[crc & 0xFF]);
   }
   return (uint16_t)~crc;
}

/* ---- L1: talk to the core ----------------------------------------------- */

static void cart_inject(const uint8_t *buf, unsigned len)
{
   if (g_cb && g_cb->receive)
      g_cb->receive(buf, len, 0);      /* from client_id 0 = the cart */
}

static void rfu_send(unsigned ptype, uint32_t hdata,
                     const uint8_t *pl, unsigned pl_len)
{
   uint8_t f[RFU1_HDR + 128];

   if (pl_len > sizeof(f) - RFU1_HDR)
      return;
   pk32be(f + 0, RFU1_MAGIC);
   pk32be(f + 4, ptype);
   pk32be(f + 8, hdata);
   if (pl_len && pl)
      memcpy(f + RFU1_HDR, pl, pl_len);
   cart_inject(f, RFU1_HDR + pl_len);
}

/* The core calls this to send us a frame.  Everything the game says arrives
 * here, wrapped as NET_RFU_CLIENT_SEND. */
static void RETRO_CALLCONV cart_core_send(int flags, const void *buf,
                                          size_t len, uint16_t client_id)
{
   const uint8_t *b = (const uint8_t *)buf;
   uint32_t ptype, hdata;

   (void)flags; (void)client_id;
   if (!C.active || len < RFU1_HDR || up32be(b) != RFU1_MAGIC)
      return;
   ptype = up32be(b + 4);
   hdata = up32be(b + 8);

   switch (ptype)
   {
   case NET_RFU_CONNECT_REQ:
   {
      if (!C.have_card || C.linked || C.connect_pending || hdata != MGC_OUR_DEVID)
         break;
      /* Each connection starts a fresh protocol session; keep the uploaded
       * parcel so a cancelled/retried in-game search can reconnect. */
      memset(&C, 0, sizeof(C));
      C.active = C.have_card = 1;
      memcpy(C.card, P.card, CARD_BYTES);
      /* rfu.c's host answers with devid | (slot << 16).  Slot 0: we allow one
       * client, which is all a distribution session ever has. */
      C.st.connects++;
      /* hdata = assigned devid | (slot << 16), as rfu.c's host builds it
       * (rfu.c:1952).  Slot 0: a distribution session has exactly one client. */
      C.devid = (int)MGC_CLIENT_DEVID;
      /* receive() is synchronous: the core enters CONNECTING only after this
       * callback returns. Deliver the ACK from the next frontend frame. */
      C.connect_pending = 1;
      fe_evt("mgc_connect devid=%04X", C.devid);
      break;
   }

   case NET_RFU_CLIENT_SEND:
   {
      /* hdata = devid | clid<<16 | blen<<24 (rfu.c:2088).  The payload is one
       * librfu subframe, including its two-byte child header. */
      unsigned blen = (hdata >> 24) & 0xFF;
      /* The core sends a fixed 104-byte carrier, padded with zeroes. */
      if (C.linked && (hdata & 0xffffffu) == MGC_CLIENT_DEVID &&
          len >= RFU1_HDR + blen && blen >= 2 && blen <= 16)
      {
         C.last_rx = C.frame;
         cart_receive_llsf(b + RFU1_HDR, blen);
      }
      break;
   }

   case NET_RFU_CLIENT_ACK:
      break;
   case NET_RFU_DISCONNECT:
      C.connect_pending = C.linked = C.st.linked = 0;
      break;
   default:
      break;
   }
}

static void cart_receive_cmd(const uint8_t *cmd)
{
   C.st.cmd_rx++;
   memcpy(C.echo, cmd, sizeof(C.echo));
   C.echo[0] &= 0x1f;
   /* Command word 0 carries the opcode in its high byte; a child also
    * rotates a 3-bit counter into bits 5-7, so mask before comparing
    * (link_rfu_2.c:950). */
   {
      uint16_t c0 = up16le(C.echo);
      unsigned op = c0 & RFUCMD_MASK;
      unsigned param = c0 & 0x1F;      /* low bits, counter excluded */
      const uint8_t *data = C.echo + 2;
      if (op == RFUCMD_READY_STANDBY || op == RFUCMD_READY_CLOSE_LINK)
      {
         memcpy(C.control, C.echo, sizeof(C.control));
         C.control_pending = 1;
         if (op == RFUCMD_READY_STANDBY) { C.standby_seen = 1; C.exchange_wait = 0; }
      }

      if (op == RFUCMD_SEND_BLOCK_INIT)
      {
         unsigned count = up16le(data);
         if (C.blk_rx_busy || C.blk_rx_done || !count || count > MAX_FRAGS)
            return;
         C.blk_rx_frags = up16le(data);          /* cmd[1] = count */
         C.blk_rx_flags = 0;
         C.blk_rx_busy  = 1;
         C.blk_rx_done  = 0;
      }
      else if (op == RFUCMD_SEND_BLOCK && C.blk_rx_busy)
      {
         unsigned idx = param;
         if (idx < C.blk_rx_frags)
         {
            memcpy(C.blk_rx + idx * FRAG_BYTES, data, FRAG_BYTES);
            C.blk_rx_flags |= (1u << idx);
            if (C.blk_rx_frags &&
                C.blk_rx_flags == ((C.blk_rx_frags >= 32) ? 0xFFFFFFFFu
                                   : ((1u << C.blk_rx_frags) - 1u)))
            {
               C.blk_rx_busy = 0;
               C.blk_rx_done = 1;
               C.st.blocks_rx++;
               C.last_progress = C.frame;
               fe_evt("mgc_block_rx count=%u state=%d", C.blk_rx_frags, C.svr);
            }
         }
      }
   }
}

/* librfu low-level subframes: child headers are 2 bytes, parent headers 3.
 * NI carries name/join traffic; UNI carries the five-player command array. */
static void cart_receive_llsf(const uint8_t *data, unsigned len)
{
   while (len >= 2)
   {
      unsigned h = up16le(data), n = h & 31;
      unsigned state = (h >> 10) & 15, ack = (h >> 9) & 1;
      unsigned phase = (h >> 5) & 3, seq = (h >> 7) & 3;
      if (n + 2 > len) return;
      if (state >= 1 && state <= 3 && !ack)
      {
         C.ni_ack = (1u << 18) | (state << 14) | (1u << 13) |
                    (phase << 9) | (seq << 11);
         if (state == 3) C.name_end = 1;
      }
      else if (state == 0 && C.name_end && !C.join_stage)
      {
         C.join_stage = 1;
         fe_evt("mgc_name_complete");
      }
      else if (ack && state >= 1 && state <= 3 &&
               state == C.join_stage && phase == 0 &&
               seq == (state == 3 ? 0u : 1u))
      {
         C.join_stage++;
         C.join_wait = 0;
         fe_evt("mgc_join stage=%u", C.join_stage);
      }
      else if (state == 4 && !ack && n == COMM_SLOT_LENGTH)
      {
         C.uni_seen = 1;
         cart_receive_cmd(data + 2);
      }
      data += n + 2; len -= n + 2;
   }
}

static void RETRO_CALLCONV cart_poll_receive(void) { /* we push, never poll */ }

/* ---- L2: advertise ------------------------------------------------------ */

/* The 24 bytes of a broadcast, as librfu lays a beacon out and as rfu.c carries
 * it (6 u32 words, rfu.c:1165):
 *
 *    0..1   serialNo   -> RFU_SERIAL_WONDER_DISTRIBUTOR, the discovery gate
 *    2..14  gname[13]  -> struct RfuGameData, packed, exactly 13 bytes
 *    15     checksum  -> inverted sum of gname[0..7] and uname[0..7]
 *    16..23 uname[8]   -> the distributor's name, GBA charmap
 *
 * struct RfuGameData (link_rfu.h:103) is:
 *    u16 compatibility bitfield { language:4 hasNews:1 hasCard:1 unknown:1
 *        canLinkNationally:1 hasNationalDex:1 gameClear:1 version:4 unused:2 }
 *    u8  playerTrainerId[2]
 *    u8  partnerInfo[4]
 *    u16 { tradeSpecies:10 tradeType:6 }
 *    u8  { activity:7 startedActivity:1 }
 *    u8  { playerGender:1 tradeLevel:7 }
 *    u8  padding
 */
static void cart_broadcast(void)
{
   uint8_t d[24];
   uint16_t compat;

   memset(d, 0, sizeof(d));
   pk16le(d + 0, RFU_SERIAL_WONDER_DISTRIBUTOR);

   /* hasCard = 1 (bit 5 of the compatibility word: language:4 then hasNews:1
    * then hasCard:1, LSB-first as the ARM ABI packs bitfields).  version and
    * language are left 0: the distributor is not a game and the Mystery Gift
    * screen does not version-check a distributor, only games. */
   compat = (uint16_t)(1u << 5);
   pk16le(d + 2, compat);
   /* d[4..5] playerTrainerId, d[6..9] partnerInfo, d[10..11] trade info: all
    * zero -- none of them mean anything for a distributor. */
   d[12] = (uint8_t)(ACTIVITY_WONDER_CARD & 0x7F);   /* activity:7 */
   /* d[13] gender/level, d[14] padding: zero. */

   /* uname at block byte 16, NOT 15 -- byte 15 is the checksum (below).
    * The name shown beside the gift.  GBA charmap, 'A' = 0xBB. */
   {
      static const char nm[] = "GIFT";
      unsigned i;
      for (i = 0; i < sizeof(nm) - 1 && i < RFU_USER_NAME_LENGTH; i++)
         d[16 + i] = (uint8_t)(0xBB + (nm[i] - 'A'));
      for (; i < RFU_USER_NAME_LENGTH; i++)
         d[16 + i] = 0xFF;                  /* EOS pad */
   }

   /* THE BEACON CHECKSUM -- without it the game discards us before it ever
    * looks at serialNo, which is exactly what "searching forever" was.
    *
    * librfu_rfu.c:707 (rfu_STC_readParentCandidateList) walks each 28-byte
    * candidate record and gates the whole thing on:
    *
    *     uname_p   = packet_p + 6;     // gname
    *     packet_p += 19;
    *     check_sum = ~*packet_p;       // record byte 19
    *     ++packet_p;                   // uname
    *     for (j = 0; j < 8; ++j) {
    *         my_check_sum += *packet_p++;   // uname[0..7]
    *         my_check_sum += *uname_p++;    // gname[0..7]
    *     }
    *     if (my_check_sum == check_sum) { ...accept... }
    *
    * Note it sums only the FIRST EIGHT bytes of gname, all eight of uname, in
    * u8 wraparound, and stores the BITWISE NOT.  Our 24-byte block is record
    * bytes 4..27, so gname[0..7] is d[2..9], uname[0..7] is d[16..23] and the
    * checksum byte is d[15]. */
   {
      uint8_t chk = 0;
      unsigned i;
      for (i = 2; i < 10; i++)
         chk = (uint8_t)(chk + d[i]);       /* gname[0..7] */
      for (i = 16; i < 24; i++)
         chk = (uint8_t)(chk + d[i]);       /* uname[0..7] */
      d[15] = (uint8_t)~chk;
   }

   /* WORD-SWAP BEFORE SENDING -- the bug that made the game never see us.
    *
    * rfu.c decodes this payload with upack32, which is BIG-endian:
    *
    *     rfu_peer_bcst[id].data[j] = upack32(&payl[j*4]);   // rfu.c:1927
    *
    * and then hands those u32s to the guest verbatim (memcpy out of a u32
    * rfu_buf, rfu.c:1165).  The GBA is little-endian, so librfu reads each word
    * back as bytes in the opposite order to the one we wrote them in: a payload
    * of [b0 b1 b2 b3] reaches the game as [b3 b2 b1 b0].
    *
    * serialNo lives in the first two bytes, so it was arriving in the wrong
    * half of the first word AND byte-swapped -- it could never equal
    * RFU_SERIAL_WONDER_DISTRIBUTOR, and Rfu_GetWonderDistributorPlayerData
    * rejected us every time.  The card still reached the PSP, which is why this
    * looked like "the gift arrives but the game does not see a cart".
    *
    * So emit each 4-byte group big-endian from the little-endian value we just
    * built: upack32 then undoes it and the guest sees `d` exactly as laid out
    * above.  Only BROADCAST needs this -- HOST_SEND is memcpy'd byte-for-byte
    * (rfu.c:2069), so command slots must NOT be swapped. */
   {
      uint8_t w[24];
      unsigned j;
      for (j = 0; j < sizeof(w); j += 4)
      {
         w[j + 0] = d[j + 3];
         w[j + 1] = d[j + 2];
         w[j + 2] = d[j + 1];
         w[j + 3] = d[j + 0];
      }
      rfu_send(NET_RFU_BROADCAST, MGC_OUR_DEVID, w, sizeof(w));
   }
   C.st.bcasts++;
}

/* ---- L2.5: send one command slot --------------------------------------- */

static void cart_send_cmd(uint16_t cmd0, const uint8_t *data12)
{
   uint8_t *slot = C.parent_cmd;

   memset(slot, 0, COMM_SLOT_LENGTH);
   pk16le(slot, cmd0);
   if (data12)
      memcpy(slot + 2, data12, FRAG_BYTES);
   /* rfu.c's host path reads the length from hdata & 0x7f (rfu.c:2030). */
   C.st.cmd_tx++;
}

/* TELL THE CLIENT WHO IT IS.
 *
 * Until the parent sends this, the game has no multiplayerId: the child reads
 * it straight out of our payload (link_rfu_2.c:1138),
 *
 *     gRfu.playerCount   = gRecvCmds[i][1];
 *     gRfu.multiplayerId = LoadLinkPlayerIds((u8 *)(gRecvCmds[i] + 2));
 *
 * and LoadLinkPlayerIds returns `ids[gRfu.childSlot]` -- the byte at the child's
 * own slot IS its id.  We hand out slot 0 in CONNECT_ACK and we are the parent
 * (id 0), so the table's first byte must be 1.  Without this the client believes
 * it is player 0, the same as us, and MysteryGiftLink's send/recv player ids
 * collide -- it would talk to itself. */
static void cart_send_player_ids(void)
{
   uint8_t d[FRAG_BYTES];

   memset(d, 0, sizeof(d));
   /* cmd[1] = player count (us + the one client). */
   pk16le(d + 0, 2);
   /* cmd[2] onward = linkPlayerIdx[RFU_CHILD_MAX], one byte per child slot.
    * Slot 0 -> multiplayer id 1; the rest are unused and stay 0. */
   d[2] = 1;
   cart_send_cmd(RFUCMD_SEND_PLAYER_IDS, d);
}

/* Start sending one link block: INIT, repeated, then the fragments. */
static void cart_block_send(const uint8_t *body, unsigned len)
{
   if (!len || len > MAX_FRAGS * FRAG_BYTES)
   {
      C.svr = SV_FAILED;
      return;
   }
   memcpy(C.blk_tx, body, len);
   C.blk_tx_len   = len;
   C.blk_tx_frags = (len + FRAG_BYTES - 1) / FRAG_BYTES;
   C.blk_tx_next  = 0;
   C.blk_tx_init_left = 3;        /* parent repeats INIT until delay > 2 */
   C.blk_tx_busy  = 1;
   fe_evt("mgc_block_tx bytes=%u first=%02x%02x", len, body[0], body[1]);
}

static void cart_block_pump(void)
{
   uint8_t d[FRAG_BYTES];

   if (!C.blk_tx_busy)
      return;

   if (C.blk_tx_init_left > 0)
   {
      /* SEND_BLOCK_INIT: cmd[1] = fragment count, cmd[2] = owner + 0x80
       * (RfuPrepareSendBuffer, link_rfu_2.c:1290).  We are the parent, so our
       * multiplayer id -- the owner -- is 0. */
      memset(d, 0, sizeof(d));
      pk16le(d + 0, (uint16_t)C.blk_tx_frags);
      pk16le(d + 2, (uint16_t)(0 + 0x80));
      cart_send_cmd(RFUCMD_SEND_BLOCK_INIT, d);
      C.blk_tx_init_left--;
      return;
   }

   if (C.blk_tx_next < C.blk_tx_frags)
   {
      unsigned off = C.blk_tx_next * FRAG_BYTES;
      unsigned n   = C.blk_tx_len - off;
      if (n > FRAG_BYTES)
         n = FRAG_BYTES;
      memset(d, 0, sizeof(d));
      memcpy(d, C.blk_tx + off, n);
      /* The fragment index rides in the LOW byte of cmd[0]. */
      cart_send_cmd((uint16_t)(RFUCMD_SEND_BLOCK | C.blk_tx_next), d);
      C.blk_tx_next++;
      if (C.blk_tx_next >= C.blk_tx_frags)
      {
         C.blk_tx_busy = 0;
         C.st.blocks_tx++;
         C.last_progress = C.frame;
      }
   }
}

/* ---- L3: a MysteryGiftLink transfer is a header block then body blocks --- */

static const uint8_t *C_mgl_body;
static unsigned       C_mgl_body_len;

static void mgl_expect(unsigned ident)
{
   C.recv_ident = ident;
   C.recv_header = 0;
   C.recv_off = 0;
}

static int mgl_receive(void)
{
   unsigned n;
   if (!C.blk_rx_done) return 0;
   C.blk_rx_done = 0;
   if (!C.recv_header)
   {
      C.recv_size = up16le(C.blk_rx + 4);
      C.recv_crc = up16le(C.blk_rx + 2);
      if (C.blk_rx_frags != 1 || up16le(C.blk_rx) != C.recv_ident ||
          !C.recv_size || C.recv_size > MGL_BUF) goto invalid;
      C.recv_header = 1;
      C.recv_off = 0;
      fe_evt("mgc_header ident=%u bytes=%u", C.recv_ident, C.recv_size);
      return 0;
   }
   n = C.recv_size - C.recv_off;
   if (n > MGL_CHUNK) n = MGL_CHUNK;
   if (C.blk_rx_frags != (n + 11) / 12) goto invalid;
   memcpy(C.recv_body + C.recv_off, C.blk_rx, n);
   C.recv_off += n;
   if (C.recv_off < C.recv_size) return 0;
   if (mgl_crc16(C.recv_body, C.recv_size) != C.recv_crc)
   {
      C.st.crc_fail++;
      goto invalid;
   }
   return 1;
invalid:
   C.svr = SV_FAILED;
   fe_evt("mgc_bad_transfer ident=%u offset=%u", C.recv_ident, C.recv_off);
   return 0;
}

static void mgl_begin_send(unsigned ident, const uint8_t *body, unsigned len)
{
   uint8_t h[MGL_HDR_SIZE];

   pk16le(h + 0, (uint16_t)ident);
   pk16le(h + 2, mgl_crc16(body, len));
   pk16le(h + 4, (uint16_t)len);
   C_mgl_body     = body;
   C_mgl_body_len = len;
   C.mgl_stage    = 0;
   C.mgl_body_off = 0;
   cart_block_send(h, MGL_HDR_SIZE);
}

/* Advance a transfer; returns 1 when the whole thing has gone out. */
static int mgl_pump_send(void)
{
   if (C.blk_tx_busy)
      return 0;
   if (C.mgl_stage == 0)
   {
      C.mgl_stage = 1;
      C.mgl_body_off = 0;
   }
   if (C.mgl_body_off < C_mgl_body_len)
   {
      unsigned n = C_mgl_body_len - C.mgl_body_off;
      if (n > MGL_CHUNK)
         n = MGL_CHUNK;
      cart_block_send(C_mgl_body + C.mgl_body_off, n);
      C.mgl_body_off += n;
      return 0;
   }
   return 1;
}

/* ---- L4: the client scripts, as exact bytes ---------------------------- */

/* struct MysteryGiftClientCmd { u32 instr; u32 parameter; } -- 8 bytes, no
 * pointers, which is what lets a script cross the link verbatim. */
static const uint8_t sc_send_game_data[] = {
   CLI_LOAD_GAME_DATA,0,0,0,  0,0,0,0,
   CLI_SEND_LOADED,   0,0,0,  0,0,0,0,
   CLI_RECV,          0,0,0,  MG_LINKID_CLIENT_SCRIPT,0,0,0,
   CLI_COPY_RECV,     0,0,0,  0,0,0,0,
};

static const uint8_t sc_save_card[] = {
   CLI_RECV,           0,0,0, MG_LINKID_CARD,0,0,0,
   CLI_SAVE_CARD,      0,0,0, 0,0,0,0,
   CLI_RECV,           0,0,0, MG_LINKID_RAM_SCRIPT,0,0,0,
   CLI_SAVE_RAM_SCRIPT,0,0,0, 0,0,0,0,
   CLI_SEND_READY_END, 0,0,0, 0,0,0,0,
   CLI_RETURN,         0,0,0, CLI_MSG_CARD_RECEIVED,0,0,0,
};

static const uint8_t sc_ask_toss[] = {
   CLI_ASK_TOSS,0,0,0, 0,0,0,0,
   CLI_LOAD_TOSS_RESPONSE,0,0,0, 0,0,0,0,
   CLI_SEND_LOADED,0,0,0, 0,0,0,0,
   CLI_RECV,0,0,0, MG_LINKID_CLIENT_SCRIPT,0,0,0,
   CLI_COPY_RECV,0,0,0, 0,0,0,0,
};
static uint8_t sc_result[] = {
   CLI_SEND_READY_END,0,0,0, 0,0,0,0,
   CLI_RETURN,0,0,0, 0,0,0,0,
};
static void svr_result(unsigned result)
{
   C.result = result;
   sc_result[12] = (uint8_t)result;
   C.svr = SV_SEND_RESULT;
   mgl_begin_send(MG_LINKID_CLIENT_SCRIPT, sc_result, sizeof(sc_result));
}

/* ---- the server ------------------------------------------------------- */

static void svr_step(void)
{
   switch (C.svr)
   {
   case SV_IDLE:
      if (C.linked)
      {
         C.svr = SV_SEND_GAMEDATA_SCRIPT;
         mgl_begin_send(MG_LINKID_CLIENT_SCRIPT, sc_send_game_data,
                        (unsigned)sizeof(sc_send_game_data));
         fe_evt("mgc_svr start card=%d", C.have_card);
      }
      break;

   case SV_SEND_GAMEDATA_SCRIPT:
      if (mgl_pump_send())
      {
         C.svr = SV_RECV_GAMEDATA;
         mgl_expect(MG_LINKID_GAME_DATA);
      }
      break;

   case SV_RECV_GAMEDATA:
      /* Match the game's compatibility fields and ROM code before choosing
       * a script: FRLG and Emerald use different event flags. */
      if (mgl_receive())
      {
         unsigned existing = up16le(C.recv_body + 20);
         if (C.recv_size != 100 || up32le(C.recv_body) != 0x101 ||
             !(C.recv_body[4] & C.recv_body[8] & 1))
         { svr_result(CLI_MSG_CANT_ACCEPT); break; }
         /* FRLG uses gift type 1/version 1 or 2. Emerald uses 4/0x200.
          * The game code is also checked; scripts are currently English-only. */
         if (up16le(C.recv_body + 12) == 1 &&
             ((up32le(C.recv_body + 16) == 1 && !memcmp(C.recv_body + 92, "BPRE", 4)) ||
              (up32le(C.recv_body + 16) == 2 && !memcmp(C.recv_body + 92, "BPGE", 4))))
         { selected_script = P.frlg; selected_script_size = P.frlg_size; }
         else if (up16le(C.recv_body + 12) == 4 && up32le(C.recv_body + 16) == 0x200 &&
                  !memcmp(C.recv_body + 92, "BPEE", 4))
         { selected_script = P.emerald; selected_script_size = P.emerald_size; }
         else { svr_result(CLI_MSG_CANT_ACCEPT); break; }
         if (!selected_script_size) { svr_result(CLI_MSG_CANT_ACCEPT); break; }
         memset(C.ram_script, 0, sizeof(C.ram_script));
         memcpy(C.ram_script, selected_script, selected_script_size);
         if (existing == up16le(C.card))
         { svr_result(CLI_MSG_HAD_CARD); break; }
         if (existing)
         {
            C.svr = SV_SEND_TOSS_SCRIPT;
            mgl_begin_send(MG_LINKID_CLIENT_SCRIPT, sc_ask_toss, sizeof(sc_ask_toss));
            break;
         }
         C.svr = SV_SEND_SAVECARD_SCRIPT;
         mgl_begin_send(MG_LINKID_CLIENT_SCRIPT, sc_save_card,
                        (unsigned)sizeof(sc_save_card));
         fe_evt("mgc_gamedata frags=%u", C.blk_rx_frags);
      }
      break;

   case SV_SEND_TOSS_SCRIPT:
      if (mgl_pump_send()) { C.svr = SV_RECV_TOSS; mgl_expect(MG_LINKID_RESPONSE); }
      break;
   case SV_RECV_TOSS:
      if (mgl_receive())
      {
         if (C.recv_size != 4 || up32le(C.recv_body))
         { svr_result(CLI_MSG_COMM_CANCELED); break; }
         C.svr = SV_SEND_SAVECARD_SCRIPT;
         mgl_begin_send(MG_LINKID_CLIENT_SCRIPT, sc_save_card, sizeof(sc_save_card));
      }
      break;
   case SV_SEND_RESULT:
      if (mgl_pump_send()) { C.svr = SV_RECV_RESULT; mgl_expect(MG_LINKID_READY_END); }
      break;
   case SV_RECV_RESULT:
      if (mgl_receive()) C.svr = SV_REFUSED;
      break;

   case SV_SEND_SAVECARD_SCRIPT:
      if (mgl_pump_send())
      {
         C.svr = SV_SEND_CARD;
         mgl_begin_send(MG_LINKID_CARD, C.card, CARD_BYTES);
      }
      break;

   case SV_SEND_CARD:
      if (mgl_pump_send())
      {
         C.svr = SV_SEND_RAMSCRIPT;
         mgl_begin_send(MG_LINKID_RAM_SCRIPT, C.ram_script, sizeof(C.ram_script));
      }
      break;

   case SV_SEND_RAMSCRIPT:
      if (mgl_pump_send())
      {
         C.svr = SV_RECV_READY_END;
         mgl_expect(MG_LINKID_READY_END);
      }
      break;

   case SV_RECV_READY_END:
      if (mgl_receive())
      {
         C.blk_rx_done = 0;
         C.svr = SV_DONE;
         fe_evt("mgc_card_sent");
      }
      break;

   case SV_REFUSED:
      break;

   case SV_DONE:
   default:
      break;
   }
   C.st.svr_state = C.svr;
}

/* ---- public API -------------------------------------------------------- */

int mgift_cart_start(void)
{
   const struct retro_netpacket_callback *cb =
      (const struct retro_netpacket_callback *)fe_host_netpacket_cb();

   if (!cb || !cb->start || !cb->receive)
   {
      fe_evt("mgc_start_fail reason=no_netpacket_iface");
      return -1;
   }

   memset(&C, 0, sizeof(C));
   memset(&P, 0, sizeof(P));
   g_cb = cb;
   C.active = 1;
   C.svr    = SV_IDLE;
   snprintf(C.status, sizeof(C.status), "advertising");

   /* We are client_id 1; the cart is 0.  connected()/disconnected() are
    * host-side-only calls per the libretro contract, so a joiner must not make
    * them (netpacket_host.c:162). */
   cb->start(1, cart_core_send, cart_poll_receive);
   fe_evt("mgc_start serial=%04X", RFU_SERIAL_WONDER_DISTRIBUTOR);
   return 0;
}

void mgift_cart_stop(void)
{
   if (!C.active)
      return;
   if (C.linked)
      rfu_send(NET_RFU_DISCONNECT, 0, NULL, 0);
   if (g_cb && g_cb->stop)
      g_cb->stop();
   fe_evt("mgc_stop bcasts=%u connects=%u cmd_tx=%u cmd_rx=%u blk_tx=%u "
          "blk_rx=%u crc_fail=%u svr=%d",
          C.st.bcasts, C.st.connects, C.st.cmd_tx, C.st.cmd_rx,
          C.st.blocks_tx, C.st.blocks_rx, C.st.crc_fail, C.svr);
   C.active = 0;
   C.linked = 0;
   g_cb = NULL;
}

int mgift_cart_active(void) { return C.active; }
int mgift_cart_has_card(void) { return C.have_card; }

int mgift_cart_set_parcel(const mgift_parcel *parcel)
{
   if (!parcel)
      return -1;
   if (C.have_card && !memcmp(&P, parcel, sizeof(P))) return 0;
   if (C.linked || C.connect_pending) return 1;
   P = *parcel;
   memcpy(C.card, P.card, CARD_BYTES);
   C.have_card = 1;
   fe_evt("mgc_card_loaded bytes=%d", CARD_BYTES);
   return 0;
}

const char *mgift_cart_status(void)
{
   if (!C.active)
      return "off";
   switch (C.svr)
   {
   case SV_IDLE:  return C.linked ? "connected" :
                        C.have_card ? "advertising" : "waiting for phone";
   case SV_DONE:  return "received by game";
   case SV_REFUSED:
      return C.result == CLI_MSG_HAD_CARD ? "card already owned" :
             C.result == CLI_MSG_COMM_CANCELED ? "cancelled in game" : "gift not compatible";
   case SV_FAILED: return "transfer failed";
   default:
      snprintf(C.status, sizeof(C.status), "sending %u/%u",
               C.blk_tx_next, C.blk_tx_frags ? C.blk_tx_frags : 1u);
      return C.status;
   }
}

void mgift_cart_get_stats(mgift_cart_stats *out)
{
   if (out)
      *out = C.st;
}

void mgift_cart_frame(void)
{
   uint8_t ll[100];
   unsigned size = 0, h;
   if (!C.active)
      return;
   C.frame++;
   if (C.connect_pending)
   {
      C.connect_pending = 0;
      rfu_send(NET_RFU_CONNECT_ACK, MGC_CLIENT_DEVID, NULL, 0);
      C.linked = C.st.linked = 1;
      C.last_rx = C.last_progress = C.frame;
      return;
   }

   /* Advertise about twice a second while nobody is connected -- the same
    * cadence rfu.c's own host uses (BCST_ANNOUNCE_VB = 30 frames). */
   if (C.have_card && !C.linked && C.frame - C.last_bcast >= 30u)
   {
      C.last_bcast = C.frame;
      cart_broadcast();
   }

   if (!C.linked) return;
   /* End a broken session through the same peer-disconnect path as RFU.
    * A toss-card prompt can wait indefinitely while the game keeps polling;
    * it must not be mistaken for a stalled transfer. */
   if (C.svr == SV_FAILED || C.frame - C.last_rx > 600u ||
       (C.svr != SV_RECV_TOSS && C.svr != SV_DONE && C.svr != SV_REFUSED &&
        C.frame - C.last_progress > 1800u))
   {
      C.svr = C.st.svr_state = SV_FAILED;
      rfu_send(NET_RFU_DISCONNECT, 0, NULL, 0);
      C.linked = C.st.linked = 0;
      return;
   }
   if (C.ni_ack)
   {
      h = C.ni_ack; C.ni_ack = 0;
      ll[size++] = (uint8_t)h; ll[size++] = (uint8_t)(h >> 8);
      ll[size++] = (uint8_t)(h >> 16);
   }
   if (C.join_stage >= 1 && C.join_stage <= 3)
   {
      /* NI control is dataType:u8, payloadSize:u16, dataSize:u32.
       * One byte of user data grants RFU_STATUS_JOIN_GROUP_OK (5). */
      static const uint8_t control[7] = {0, 7, 0, 1, 0, 0, 0};
      unsigned n = C.join_stage == 1 ? 7 : C.join_stage == 2 ? 1 : 0;
      h = (1u << 18) | (C.join_stage << 14) |
          (C.join_stage == 3 ? 0u : 1u << 11) | n;
      ll[size++] = (uint8_t)h; ll[size++] = (uint8_t)(h >> 8);
      ll[size++] = (uint8_t)(h >> 16);
      if (C.join_stage == 1) { memcpy(ll + size, control, n); size += n; }
      if (C.join_stage == 2) ll[size++] = 5;
   }
   else if (C.join_stage == 4 && ++C.join_wait > 8)
   {
      memset(C.parent_cmd, 0, sizeof(C.parent_cmd));
      if (C.control_pending)
      {
         memcpy(C.parent_cmd, C.control, sizeof(C.parent_cmd));
         C.control_pending = 0;
      }
      else
      {
         if (!C.exchange)
         {
            cart_send_player_ids();
            if (C.uni_seen && ++C.exchange_wait > 8) C.exchange = 1;
         }
         else if (C.exchange == 1)
         {
            uint8_t d[12] = {0};
            cart_send_cmd(RFUCMD_SEND_BLOCK_REQ, d);
            C.exchange = 2;
         }
         else if (C.exchange == 2 && C.blk_rx_done)
         {
            /* Exchange the standard LinkPlayerBlock before starting MGL. */
            uint8_t player[200] = {0};
            memcpy(player, C.blk_rx, 60);
            memset(player + 24, 0xff, 8);
            player[24] = 0xc1; player[25] = 0xc3; /* GI */
            player[26] = 0xc0; player[27] = 0xce; /* FT */
            pk16le(player + 40, 0);
            C.blk_rx_done = 0;
            cart_block_send(player, sizeof(player));
            C.exchange = 3;
            fe_evt("mgc_player_exchange");
         }
         else if (C.exchange == 3 && !C.blk_tx_busy && C.standby_seen)
         {
            if (++C.exchange_wait > 8) C.exchange = 4;
         }
         cart_block_pump();
         if (C.exchange == 4) svr_step();
      }
      h = (1u << 18) | (4u << 14) | 70;
      ll[size++] = (uint8_t)h; ll[size++] = (uint8_t)(h >> 8);
      ll[size++] = (uint8_t)(h >> 16);
      memset(ll + size, 0, 70);
      memcpy(ll + size, C.parent_cmd, 14);
      memcpy(ll + size + 14, C.echo, 14);
      memset(C.echo, 0, sizeof(C.echo));
      size += 70;
   }
   if (!size)
   {
      /* A nonempty NULL subframe completes NI's END/NULL handshake. */
      ll[0] = ll[1] = 0; ll[2] = 4; size = 3;
   }
   rfu_send(NET_RFU_HOST_SEND, size, ll, size);
}

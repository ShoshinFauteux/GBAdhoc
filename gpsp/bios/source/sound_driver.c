/* sound_driver.c -- the BIOS-resident sound driver (SWI 19h-1Eh, 28h, 29h)
 * for the open-source replacement BIOS.
 *
 * Copyright (C) 2026 GBAdhoc contributors.  GPL v2 or later, like the rest of
 * this BIOS (bios/README.md).
 *
 * Written from the public description of the interface (GBATEK, "BIOS Sound
 * Functions": the SoundArea layout, the SWI argument encodings, the PCM rate
 * table) and checked black-box against the behaviour of the console BIOS in
 * an emulator (same game, same inputs: driver fields, channel state and the
 * PCM ring compared frame by frame).  No Nintendo code is used.
 *
 * Some games do not link a sound engine of their own: they keep their
 * sequencer in the cartridge and leave the PCM mixing to the BIOS, through
 * SoundDriverInit / SoundDriverMode / SoundDriverMain / SoundDriverVSync.
 * The earlier open BIOS answered all of them with a bare return, so the
 * sound hardware was never switched on and those games ran silent (X-Men:
 * Reign of Apocalypse).  This file implements the direct-sound part of the
 * driver:
 *
 *   19h SoundBias          bias level 0 or 200h (immediately, no ramp)
 *   1Ah SoundDriverInit    hardware set-up, area cleared, 13379 Hz
 *   1Bh SoundDriverMode    reverb, channel count, volume, rate, DAC bits
 *   1Ch SoundDriverMain    the PCM mixer (envelopes, pitch, loops, reverb)
 *   1Dh SoundDriverVSync   restarts the PCM ring DMA once per lap
 *   1Eh SoundChannelClear  silences every channel
 *   28h SoundDriverVSyncOff / 29h SoundDriverVSyncOn
 *
 * Not implemented (unchanged from before): the BIOS's own music player
 * (20h-24h), its per-command sequencer routines (the MPlayJumpTable /
 * SoundGetJumpList entries and `plynote`), which stay harmless returns.  A
 * game that runs its sequencer through those still gets no notes; a game
 * that drives the channels itself (as X-Men does) gets its full sound.
 *
 * Constraints: ARMv4T, ARM state, called from swi_vector in system mode
 * with the caller's IRQ mask; r0-r3/r12/lr may be clobbered.  No library
 * calls: no division (the rate table is folded at compile time), no memset.
 */
typedef unsigned char u8;
typedef signed char s8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed int s32;

#define REG16(a) (*(volatile u16 *)(a))
#define REG8(a) (*(volatile u8 *)(a))
#define REG32(a) (*(volatile u32 *)(a))

#define SOUNDCNT_H REG16(0x04000082)
#define SOUNDCNT_X REG16(0x04000084)
#define SOUNDBIAS REG16(0x04000088)
#define SOUNDBIAS_H REG8(0x04000089)
#define DMA1SAD REG32(0x040000BC)
#define DMA1DAD REG32(0x040000C0)
#define DMA1CNT REG32(0x040000C4)
#define DMA1CNT_H REG16(0x040000C6)
#define DMA2SAD REG32(0x040000C8)
#define DMA2DAD REG32(0x040000CC)
#define DMA2CNT REG32(0x040000D0)
#define DMA2CNT_H REG16(0x040000D2)
#define TM0CNT_L REG16(0x04000100)
#define TM0CNT_H REG16(0x04000102)
#define VCOUNT REG8(0x04000006)
#define SOUND_AREA_PTR (*(SoundArea *volatile *)0x03007FF0)

#define DMA_SOUND_RING 0xB600 /* enable, FIFO timing, 32-bit, repeat */
#define DMA_32BIT 0x0400
#define DMA_REPEAT_HI (0x0200u << 16)

#define ID_SMSH 0x68736D53u   /* "SmsH": unlocked */
#define PCM_RING 0x630        /* bytes per half (right, then left) */

/* SoundChannel.status */
#define SF_START 0x80
#define SF_STOP 0x40
#define SF_LOOP 0x10
#define SF_ECHO 0x04
#define SF_ENV 0x03
#define SF_ATTACK 3
#define SF_DECAY 2
#define SF_ON (SF_START | SF_STOP | SF_ECHO | SF_ENV)

typedef struct {
  u16 type;
  u16 status;      /* bit 14 (and 15): loops */
  u32 freq;        /* Hz * 1024 */
  u32 loop_start;
  u32 size;
  s8 data[1];
} WaveData;

typedef struct {
  u8 status;       /* 00 */
  u8 type;         /* 01 bit 3: fixed rate (one input sample per output) */
  u8 right_vol;    /* 02 */
  u8 left_vol;     /* 03 */
  u8 attack;       /* 04 */
  u8 decay;        /* 05 */
  u8 sustain;      /* 06 */
  u8 release;      /* 07 */
  u8 key;          /* 08 */
  u8 env_vol;      /* 09 */
  u8 env_right;    /* 0A */
  u8 env_left;     /* 0B */
  u8 echo_vol;     /* 0C */
  u8 echo_len;     /* 0D */
  u8 pad0[10];     /* 0E-17: sequencer bytes, untouched */
  s32 count;       /* 18 samples left before the end of the wave */
  u32 fw;          /* 1C position fraction, 23 bits */
  u32 frequency;   /* 20 output rate in Hz (MidiKey2Freq) */
  WaveData *wav;   /* 24 */
  s8 *cur;         /* 28 */
  u32 pad1[5];     /* 2C-3F: sequencer links, untouched */
} SoundChannel;

typedef void (*VoidFn)(void);
typedef void (*ArgFn)(u32);

typedef struct {
  volatile u32 ident;   /* 00 */
  volatile u8 dma_counter; /* 04 */
  u8 reverb;            /* 05 */
  u8 max_chans;         /* 06 */
  u8 master_vol;        /* 07 */
  u8 freq;              /* 08 rate index 1-12 */
  u8 mode;              /* 09 */
  u8 c15;               /* 0A */
  u8 dma_period;        /* 0B ring laps in frames */
  u8 max_lines;         /* 0C */
  u8 pad0[3];
  s32 spv;              /* 10 samples per frame */
  s32 pcm_freq;         /* 14 */
  s32 div_freq;         /* 18 2^23 / pcm_freq */
  void *cgb_chans;      /* 1C */
  ArgFn main_head;      /* 20 the game's sequencer, called first */
  u32 intp;             /* 24 its argument */
  VoidFn cgb_sound;     /* 28 */
  ArgFn cgb_osc_off;    /* 2C */
  VoidFn key_to_cgb;    /* 30 */
  const void *jump_table; /* 34 */
  VoidFn plynote;       /* 38 */
  VoidFn ext_vol_pit;   /* 3C */
  u8 pad1[16];          /* 40 */
  SoundChannel chans[12]; /* 50 */
  s8 pcm[PCM_RING * 2]; /* 350 */
} SoundArea;

typedef char check_chan[(sizeof(SoundChannel) == 0x40) ? 1 : -1];
typedef char check_area[(sizeof(SoundArea) == 0xFB0) ? 1 : -1];

/* The rate table: index 1-12.  Every value is folded by the compiler. */
#define PCMF(s) ((597275 * (s) + 5000) / 10000)
#define DIVF(s) ((16777216 / PCMF(s) + 1) >> 1)
#define ROW(s) {s, PCMF(s), DIVF(s), 280896 / (s), PCM_RING / (s)}
typedef struct { u16 spv, pcm_freq, div_freq, timer, period; } Rate;
static const Rate rates[12] = {ROW(96), ROW(132), ROW(176), ROW(224), ROW(264), ROW(304),
                               ROW(352), ROW(448), ROW(528), ROW(608), ROW(672), ROW(704)};

/* The default callbacks and sequencer table: harmless returns. */
static void nothing(void) {}
static const VoidFn null_jump_table[36] = {
  nothing, nothing, nothing, nothing, nothing, nothing, nothing, nothing, nothing,
  nothing, nothing, nothing, nothing, nothing, nothing, nothing, nothing, nothing,
  nothing, nothing, nothing, nothing, nothing, nothing, nothing, nothing, nothing,
  nothing, nothing, nothing, nothing, nothing, nothing, nothing, nothing, nothing};

static void fill32(void *dst, u32 bytes)
{
  volatile u32 *d = dst;
  for (bytes >>= 2; bytes; bytes--)
    *d++ = 0;
}

void sd_SoundBias(u32 level)
{
  SOUNDBIAS = (SOUNDBIAS & 0xC000) | (level ? 0x200 : 0);
}

void sd_VSyncOff(void)
{
  SoundArea *a = SOUND_AREA_PTR;
  if (a->ident - ID_SMSH > 1)
    return;
  a->ident += 10;
  if (DMA1CNT & DMA_REPEAT_HI)
    DMA1CNT = (0x8400u << 16) | 4;   /* drain: four words, now */
  if (DMA2CNT & DMA_REPEAT_HI)
    DMA2CNT = (0x8400u << 16) | 4;
  DMA1CNT_H = DMA_32BIT;
  DMA2CNT_H = DMA_32BIT;
  fill32(a->pcm, sizeof(a->pcm));
}

void sd_VSyncOn(void)
{
  SoundArea *a = SOUND_AREA_PTR;
  if (a->ident - (ID_SMSH + 10) > 1)
    return;
  DMA1CNT_H = DMA_SOUND_RING;
  DMA2CNT_H = DMA_SOUND_RING;
  a->dma_counter = 0;
  a->ident -= 10;
}

static void set_rate(SoundArea *a, u32 index)
{
  const Rate *r = &rates[index - 1];
  a->freq = index;
  a->spv = r->spv;
  a->dma_period = r->period;
  a->pcm_freq = r->pcm_freq;
  a->div_freq = r->div_freq;
  TM0CNT_H = 0;
  TM0CNT_L = -r->timer;
  sd_VSyncOn();
  while (VCOUNT == 159)
    ;
  while (VCOUNT != 159)
    ;
  TM0CNT_H = 0x80;
}

void sd_SoundDriverInit(SoundArea *a)
{
  DMA1CNT_H = DMA_32BIT;
  DMA2CNT_H = DMA_32BIT;
  SOUNDCNT_X = 0x8F;
  SOUNDCNT_H = 0xA90E;
  SOUNDBIAS_H = (SOUNDBIAS_H & 0x3F) | 0x40;
  DMA1SAD = (u32)a->pcm;
  DMA1DAD = 0x040000A0;
  DMA2SAD = (u32)a->pcm + PCM_RING;
  DMA2DAD = 0x040000A4;
  SOUND_AREA_PTR = a;
  fill32(a, sizeof(*a));
  a->max_chans = 8;
  a->master_vol = 15;
  a->cgb_sound = nothing;
  a->cgb_osc_off = (ArgFn)nothing;
  a->key_to_cgb = nothing;
  a->ext_vol_pit = nothing;
  a->plynote = nothing;
  a->jump_table = null_jump_table;
  set_rate(a, 4);
  a->ident = ID_SMSH;
}

void sd_SoundDriverMode(u32 mode)
{
  SoundArea *a = SOUND_AREA_PTR;
  u32 v;
  if (a->ident != ID_SMSH)
    return;
  a->ident++;
  if (mode & 0x80)
    a->reverb = mode & 0x7F;
  if ((v = mode & 0xF00)) {
    int i;
    a->max_chans = v >> 8;
    for (i = 0; i < 12; i++)
      a->chans[i].status = 0;
  }
  if ((v = mode & 0xF000))
    a->master_vol = v >> 12;
  if ((v = mode & 0xB00000))
    SOUNDBIAS_H = (SOUNDBIAS_H & 0x3F) | ((v & 0x300000) >> 14);
  if ((v = (mode >> 16) & 0xF) && v <= 12) {
    sd_VSyncOff();
    set_rate(a, v);
  }
  a->ident = ID_SMSH;
}

void sd_SoundChannelClear(void)
{
  SoundArea *a = SOUND_AREA_PTR;
  int i;
  if (a->ident != ID_SMSH)
    return;
  a->ident++;
  for (i = 0; i < 12; i++)
    a->chans[i].status = 0;
  if (a->cgb_chans) {
    u8 *c = a->cgb_chans;
    for (i = 1; i <= 4; i++, c += 0x40) {
      a->cgb_osc_off(i);
      *c = 0;
    }
  }
  a->ident = ID_SMSH;
}

void sd_SoundDriverVSync(void)
{
  SoundArea *a = SOUND_AREA_PTR;
  if (a->ident - ID_SMSH > 1)
    return;
  if ((s8)--a->dma_counter > 0)
    return;
  a->dma_counter = a->dma_period;
  if (DMA1CNT & DMA_REPEAT_HI)
    DMA1CNT_H = DMA_32BIT;
  if (DMA2CNT & DMA_REPEAT_HI)
    DMA2CNT_H = DMA_32BIT;
  DMA1CNT_H = DMA_SOUND_RING;
  DMA2CNT_H = DMA_SOUND_RING;
}

/* One frame of the envelope; returns 0 when the channel stops. */
static int envelope(SoundChannel *c, const SoundArea *a)
{
  u32 st = c->status;
  u32 env;
  if (st & SF_START) {
    WaveData *w = c->wav;
    if (st & SF_STOP)
      return 0;
    st = SF_ATTACK;
    c->cur = w->data;
    c->count = w->size;
    c->fw = 0;
    env = 0;
    if (w->status & 0xC000)
      st |= SF_LOOP;
    goto attack;
  }
  env = c->env_vol;
  if (st & SF_ECHO) {
    if (--c->echo_len == 0)
      return 0;
  } else if (st & SF_STOP) {
    env = (env * c->release) >> 8;
    if (env <= c->echo_vol)
      goto echo;
  } else if ((st & SF_ENV) == SF_DECAY) {
    env = (env * c->decay) >> 8;
    if (env <= c->sustain) {
      env = c->sustain;
      if (env == 0)
        goto echo;
      st--;
    }
  } else if ((st & SF_ENV) == SF_ATTACK) {
attack:
    env += c->attack;
    if (env >= 0xFF) {
      env = 0xFF;
      st--;
    }
  }
  goto store;
echo:
  env = c->echo_vol;
  if (env == 0)
    return 0;
  st |= SF_ECHO;
store:
  c->status = st;
  c->env_vol = env;
  env = ((a->master_vol + 1) * env) >> 4;
  c->env_right = (c->right_vol * env) >> 8;
  c->env_left = (c->left_vol * env) >> 8;
  return 1;
}

/* Mix one channel into this frame's slice of the ring. */
static void mix(SoundChannel *c, const SoundArea *a, s8 *out, s32 n)
{
  const WaveData *w = c->wav;
  s32 vr = c->env_right, vl = c->env_left;
  s32 count = c->count;
  s8 *p = c->cur;
  s32 loop_len = (c->status & SF_LOOP) ? (s32)(w->size - w->loop_start) : 0;

  if (c->type & 0x08) {           /* fixed rate */
    for (; n > 0; n--, out++) {
      s32 v = *p++;
      out[0] += (v * vr) >> 8;
      out[PCM_RING] += (v * vl) >> 8;
      if (--count <= 0) {
        if (!loop_len) {
          c->status = 0;
          break;
        }
        count += loop_len;
        p -= loop_len;
      }
    }
  } else {
    u32 fw = c->fw;
    u32 step = c->frequency * a->div_freq;
    for (; n > 0; n--, out++) {
      s32 s0 = p[0];
      s32 v = s0 + (((s32)(p[1] - s0) * (s32)fw) >> 23);
      u32 adv;
      out[0] += (v * vr) >> 8;
      out[PCM_RING] += (v * vl) >> 8;
      fw += step;
      adv = fw >> 23;
      if (!adv)
        continue;
      fw &= 0x7FFFFF;
      count -= adv;
      p += adv;
      if (count <= 0) {
        if (!loop_len) {
          c->status = 0;
          break;
        }
        do {
          count += loop_len;
          p -= loop_len;
        } while (count <= 0);
      }
    }
    c->fw = fw;
  }
  c->count = count;
  c->cur = p;
}

void sd_SoundDriverMain(void)
{
  SoundArea *a = SOUND_AREA_PTR;
  s32 spv, i, c;
  s8 *slice;

  if (a->ident != ID_SMSH)
    return;
  a->ident++;
  if (a->main_head)
    a->main_head(a->intp);
  a->cgb_sound();

  spv = a->spv;
  c = a->dma_counter;
  slice = a->pcm;
  if (c >= 2)
    slice += (a->dma_period - c + 1) * spv;

  if (a->reverb) {
    /* the slice still holds the previous lap: fold it with the next slice */
    s8 *next = (c == 2) ? a->pcm : slice + spv;
    s32 r = a->reverb;
    for (i = 0; i < spv; i++) {
      s32 v = slice[i] + slice[i + PCM_RING] + next[i] + next[i + PCM_RING];
      v = (v * r) >> 9;
      if (v & 0x80)
        v++;
      slice[i] = v;
      slice[i + PCM_RING] = v;
    }
  } else {
    fill32(slice, spv);
    fill32(slice + PCM_RING, spv);
  }

  for (i = 0; i < a->max_chans; i++) {
    SoundChannel *ch = &a->chans[i];
    if (!(ch->status & SF_ON))
      continue;
    if (!envelope(ch, a)) {
      ch->status = 0;
      continue;
    }
    mix(ch, a, slice, spv);
  }
  a->ident = ID_SMSH;
}

#include "../gbcore.h"

#include <stdio.h>
#include <string.h>

typedef struct smoke_stats {
  unsigned video_frames;
  size_t audio_frames;
  unsigned serial_transfers;
  uint8_t serial_outgoing;
  int serial_internal_clock;
  unsigned width;
  unsigned height;
} smoke_stats_t;

static void on_video(void *userdata, const gbcore_video_frame_t *frame)
{
  smoke_stats_t *stats = (smoke_stats_t *)userdata;
  if (frame && frame->pixels && frame->pitch_bytes >= frame->width * 2) {
    stats->video_frames++;
    stats->width = frame->width;
    stats->height = frame->height;
  }
}

static void on_audio(void *userdata, const int16_t *samples, size_t frames)
{
  smoke_stats_t *stats = (smoke_stats_t *)userdata;
  if (samples || frames == 0) stats->audio_frames += frames;
}

static int on_serial_transfer(void *userdata, uint8_t outgoing,
                              int internal_clock, uint8_t *received)
{
  smoke_stats_t *stats = (smoke_stats_t *)userdata;
  stats->serial_transfers++;
  stats->serial_outgoing = outgoing;
  stats->serial_internal_clock = internal_clock;
  *received = 0xA5;
  return 1;
}

static void make_rom(unsigned char rom[0x8000], int color)
{
  static const unsigned char nintendo_logo[48] = {
    0xCE,0xED,0x66,0x66,0xCC,0x0D,0x00,0x0B,0x03,0x73,0x00,0x83,
    0x00,0x0C,0x00,0x0D,0x00,0x08,0x11,0x1F,0x88,0x89,0x00,0x0E,
    0xDC,0xCC,0x6E,0xE6,0xDD,0xDD,0xD9,0x99,0xBB,0xBB,0x67,0x63,
    0x6E,0x0E,0xEC,0xCC,0xDD,0xDC,0x99,0x9F,0xBB,0xB9,0x33,0x3E
  };
  memset(rom, 0, 0x8000);
  /* A real entry jump and a tiny program that enables LCD then idles. */
  rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 0x01; /* JP $0150 */
  rom[0x134] = 'G'; rom[0x135] = 'B'; rom[0x136] = 'A';
  rom[0x137] = 'D'; rom[0x138] = 'H'; rom[0x139] = 'O';
  rom[0x13A] = 'C'; rom[0x13B] = ' '; rom[0x13C] = 'T';
  rom[0x13D] = 'E'; rom[0x13E] = 'S'; rom[0x13F] = 'T';
  rom[0x143] = color ? 0x80 : 0x00;
  memcpy(rom + 0x104, nintendo_logo, sizeof(nintendo_logo));
  rom[0x147] = 0x00;
  rom[0x148] = 0x00;
  rom[0x149] = 0x00;
  rom[0x14A] = 0x01;
  unsigned char check = 0;
  for (unsigned i = 0x134; i <= 0x14C; i++) check = (unsigned char)(check - rom[i] - 1);
  rom[0x14D] = check;
  unsigned sum = 0;
  for (unsigned i = 0; i < 0x8000; i++) if (i != 0x14E && i != 0x14F) sum += rom[i];
  rom[0x14E] = (unsigned char)(sum >> 8);
  rom[0x14F] = (unsigned char)sum;
  rom[0x150] = 0x31; rom[0x151] = 0xFE; rom[0x152] = 0xFF; /* LD SP,$FFFE */
  rom[0x153] = 0x3E; rom[0x154] = 0x91;                    /* LD A,$91 */
  rom[0x155] = 0xE0; rom[0x156] = 0x40;                    /* LDH ($40),A */
  rom[0x157] = 0x3E; rom[0x158] = 0x3C;                    /* LD A,$3C */
  rom[0x159] = 0xE0; rom[0x15A] = 0x01;                    /* LDH (SB),A */
  rom[0x15B] = 0x3E; rom[0x15C] = color ? 0x81 : 0x80;     /* start transfer */
  rom[0x15D] = 0xE0; rom[0x15E] = 0x02;                    /* LDH (SC),A */
  rom[0x15F] = 0x18; rom[0x160] = 0xFE;                    /* JR -2 */
}

static int run_one(fe_console_t console, int color)
{
  unsigned char rom[0x8000];
  make_rom(rom, color);
  smoke_stats_t stats = {0};
  gbcore_callbacks_t callbacks = {0};
  callbacks.userdata = &stats;
  callbacks.video = on_video;
  callbacks.audio_batch = on_audio;
  callbacks.serial_transfer = on_serial_transfer;
  gbcore_t *core = gbcore_create(NULL, rom, sizeof(rom), console, 48000, &callbacks);
  if (!core) {
    fprintf(stderr, "gbcore_create failed for console %d\n", console);
    return 1;
  }
  if (gbcore_frame_width(core) != 160 || gbcore_frame_height(core) != 144 ||
      gbcore_sample_rate(core) != 48000) {
    fprintf(stderr, "unexpected geometry/rate for console %d\n", console);
    gbcore_shutdown(core);
    return 1;
  }
  /* TGB Dual starts at the cartridge entry point (no boot ROM); run well
   * past it and require the ROM's serial instruction. */
  for (unsigned i = 0; i < 360; i++) {
    if (gbcore_run_frame(core, 0) != 0) {
      fprintf(stderr, "frame execution failed for console %d\n", console);
      gbcore_shutdown(core);
      return 1;
    }
  }
  char title[17];
  if (gbcore_rom_title(core, title, sizeof(title)) != 0 ||
      strcmp(title, "GBADHOC TEST") != 0 || gbcore_frame_number(core) != 360 ||
      stats.video_frames != 360 ||
      stats.serial_transfers == 0 ||
      stats.serial_outgoing != 0x3C ||
      stats.serial_internal_clock != color) {
    fprintf(stderr, "ROM did not run/render as expected: console %d, title=%s, frames=%u, serial=%u/%02x/%d\n",
            console, title, stats.video_frames, stats.serial_transfers,
            stats.serial_outgoing, stats.serial_internal_clock);
    gbcore_shutdown(core);
    return 1;
  }
  printf("console=%s title=%s frames=%llu video=%ux%u audio_pairs=%lu serial=%u/%02x/%s\n",
         color ? "GBC" : "GB", title,
         (unsigned long long)gbcore_frame_number(core), stats.width, stats.height,
         (unsigned long)stats.audio_frames, stats.serial_transfers,
         stats.serial_outgoing,
         stats.serial_internal_clock ? "internal" : "external");
  gbcore_shutdown(core);
  return 0;
}

int main(void)
{
  return run_one(FE_CONSOLE_GB, 0) || run_one(FE_CONSOLE_GBC, 1);
}

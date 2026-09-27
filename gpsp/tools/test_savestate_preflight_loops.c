#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../common.h"
#include "../main.h"
#include "../savestate.h"
#include "../gba_memory.h"
#include "../sound.h"

static u8 state[GBA_STATE_MEM_SIZE];
static u8 *write_ptr;
static unsigned component_reads;

static void put_u32(u8 *p, u32 value)
{
  p[0] = (u8)value;
  p[1] = (u8)(value >> 8);
  p[2] = (u8)(value >> 16);
  p[3] = (u8)(value >> 24);
}

static void put_cstring(const char *s)
{
  size_t len = strlen(s) + 1;
  memcpy(write_ptr, s, len);
  write_ptr += len;
}

static u8 *begin_doc(u8 *p, const char *name)
{
  *p++ = BSON_TYPE_DOC;
  write_ptr = p;
  put_cstring(name);
  p = write_ptr;
  put_u32(p, 0);
  write_ptr = p + 4;
  return p;
}

static u8 *end_doc(u8 *header)
{
  *write_ptr++ = 0;
  put_u32(header, (u32)(write_ptr - header));
  return write_ptr;
}

static void put_int(const char *name, u32 value)
{
  *write_ptr++ = BSON_TYPE_INT32;
  put_cstring(name);
  put_u32(write_ptr, value);
  write_ptr += 4;
}

static void put_bin(const char *name, unsigned size)
{
  *write_ptr++ = BSON_TYPE_BIN;
  put_cstring(name);
  put_u32(write_ptr, size);
  write_ptr += 4;
  *write_ptr++ = 0;
  memset(write_ptr, 0, size);
  write_ptr += size;
}

static void put_array2(const char *name)
{
  *write_ptr++ = BSON_TYPE_ARR;
  put_cstring(name);
  put_u32(write_ptr, 21);
  write_ptr += 4;
  put_int("0", 0);
  put_int("1", 0);
  *write_ptr++ = 0;
}

static void put_input_doc(void)
{
  u8 *hdr = begin_doc(write_ptr, "input");
  put_int("prevkey", 0);
  put_int("turbo-a", 0);
  put_int("turbo-b", 0);
  put_int("gbp-frames", 0);
  put_int("gbp-sent", 0);
  write_ptr = end_doc(hdr);
}

static void put_main_docs(void)
{
  unsigned i;
  u8 *hdr = begin_doc(write_ptr, "emu");
  put_int("cpu-ticks", 0);
  put_int("exec-cycles", 0);
  put_int("video-count", 0);
  put_int("sleep-cycles", 0);
  write_ptr = end_doc(hdr);

  hdr = begin_doc(write_ptr, "timers");
  for (i = 0; i < 4; i++)
  {
    char name[2] = { (char)('0' + i), 0 };
    u8 *timer_hdr = begin_doc(write_ptr, name);
    put_int("count", 0);
    put_int("reload", 0x10000);
    put_int("prescale", 0);
    put_int("freq-step", 0);
    put_int("dsc", i == 0 ? TIMER_DS_CHANNEL_BOTH : TIMER_DS_CHANNEL_NONE);
    put_int("irq", 0);
    put_int("status", TIMER_INACTIVE);
    write_ptr = end_doc(timer_hdr);
  }
  write_ptr = end_doc(hdr);
}

static void put_memory_doc(void)
{
  u8 *hdr = begin_doc(write_ptr, "memory");
  put_bin("iwram", 0x8000);
  put_bin("ewram", 0x40000);
  put_bin("vram", 1024 * 96);
  put_bin("oamram", 0x400);
  put_bin("palram", 0x400);
  put_bin("ioregs", 0x400);
  put_int("dma-bus", 0);
  write_ptr = end_doc(hdr);
}

static const char *const backup_ints[] = {
  "backup-type", "flash-mode", "flash-cmd-pos", "flash-bank-num",
  "flash-dev-id", "flash-size", "eeprom-size", "eeprom-mode",
  "eeprom-addr", "eeprom-counter", "rtc-state", "rtc-write-mode",
  "rtc-cmd", "rtc-status", "rtc-data-bit-cnt", "rtc-bit-cnt"
};

static void put_backup_doc(void)
{
  unsigned i;
  static const u32 valid_values[] = {
    BACKUP_UNKN, 0, 0, 0, FLASH_DEVICE_MACRONIX_64KB, FLASH_SIZE_64KB,
    EEPROM_512_BYTE, EEPROM_BASE_MODE, 0, 0, 0, 0, 0, 0, 0, 0
  };
  u8 *hdr = begin_doc(write_ptr, "backup");
  for (i = 0; i < sizeof(backup_ints) / sizeof(backup_ints[0]); i++)
    put_int(backup_ints[i], valid_values[i]);
  put_bin("gpio-regs", 3);
  put_array2("rtc-data-words");
  write_ptr = end_doc(hdr);
}

static const char *const dma_ints[] = {
  "src-addr", "dst-addr", "src-dir", "dst-dir", "len", "size",
  "repeat", "start", "dsc", "irq"
};

static void put_dma_doc(int bad_channel, int bad_field)
{
  int d;
  unsigned v;
  u8 *hdr = begin_doc(write_ptr, "dma");
  for (d = 0; d < DMA_CHAN_CNT; d++)
  {
    char name[2] = { (char)('0' + d), 0 };
    u8 *dhdr = begin_doc(write_ptr, name);
    for (v = 0; v < sizeof(dma_ints) / sizeof(dma_ints[0]); v++)
      if (!(d == bad_channel && v == 9))
        put_int(dma_ints[v], d == 0 && v == (unsigned)bad_field ? 0xffffffffu : 0);
    write_ptr = end_doc(dhdr);
  }
  write_ptr = end_doc(hdr);
}

static const char *const sound_ints[] = {
  "on", "buf-base", "gbc-buf-idx", "gbc-last-cpu-ticks",
  "gbc-partial-ticks", "gbc-ms-vol-left", "gbc-ms-vol-right", "gbc-ms-vol"
};
static const char *const ds_ints[] = {
  "status", "volume", "fifo-base", "fifo-top", "fifo-frac", "buf-idx"
};
static const char *const gs_ints[] = {
  "status", "rate", "freq-step", "sample-idx", "tick-cnt", "volume",
  "active", "enable", "env-vol0", "env-vol", "env-dir", "env-status",
  "env-ticks0", "env-ticks", "sweep-status", "sweep-dir", "sweep-ticks0",
  "sweep-ticks", "sweep-shift", "wav-type", "wav-bank", "wav-vol",
  "len-status", "len-ticks", "noise-type", "sample-tbl"
};

static void put_channel(const char *name, const char *const *keys,
                        unsigned count, int omit_last)
{
  unsigned i;
  u8 *hdr = begin_doc(write_ptr, name);
  for (i = 0; i < count; i++)
    if (!(omit_last && i == count - 1))
      put_int(keys[i], 0);
  if (name[0] == 'd')
    put_bin("fifo-bytes", 32);
  write_ptr = end_doc(hdr);
}

static void put_sound_doc(int bad_ds, int bad_gs)
{
  unsigned i;
  char name[4];
  u8 *hdr = begin_doc(write_ptr, "sound");
  for (i = 0; i < sizeof(sound_ints) / sizeof(sound_ints[0]); i++)
    put_int(sound_ints[i], 0);
  put_bin("wav-samples", 64);
  for (i = 0; i < 2; i++)
  {
    snprintf(name, sizeof(name), "ds%u", i);
    put_channel(name, ds_ints, sizeof(ds_ints) / sizeof(ds_ints[0]),
                (int)i == bad_ds);
  }
  for (i = 0; i < 4; i++)
  {
    snprintf(name, sizeof(name), "gs%u", i);
    put_channel(name, gs_ints, sizeof(gs_ints) / sizeof(gs_ints[0]),
                (int)i == bad_gs);
  }
  write_ptr = end_doc(hdr);
}

static void make_state(int bad_dma, int bad_ds, int bad_gs, int bad_dma_field)
{
  memset(state, 0, sizeof(state));
  put_u32(state, GBA_STATE_MEM_SIZE);
  write_ptr = state + 4;
  put_int("info-magic", GBA_STATE_MAGIC);
  put_int("info-version", GBA_STATE_VERSION);
  put_main_docs();
  put_input_doc();
  put_memory_doc();
  put_backup_doc();
  put_dma_doc(bad_dma, bad_dma_field);
  put_sound_doc(bad_ds, bad_gs);
  *write_ptr = 0;
}

/* These components are irrelevant to the malformed later-channel cases;
 * counting their read paths proves gba_load_state rejects before mutation. */
bool cpu_check_savestate(const u8 *src) { (void)src; return true; }
bool cpu_read_savestate(const u8 *src) { (void)src; component_reads++; return true; }
bool input_read_savestate(const u8 *src) { (void)src; component_reads++; return true; }
bool main_read_savestate(const u8 *src) { (void)src; component_reads++; return true; }
bool memory_read_savestate(const u8 *src) { (void)src; component_reads++; return true; }
bool sound_read_savestate(const u8 *src) { (void)src; component_reads++; return true; }

u16 palette_ram[512];
u16 palette_ram_converted[512];
u32 instruction_count;
u32 reg[64];
u32 gbp_get_state(void) { return 0; }
void rumble_restore_state(bool gbp_active) { (void)gbp_active; }
void video_reload_counters(void) {}

static int rejects_late_channel(const char *label, int bad_dma,
                                int bad_ds, int bad_gs)
{
  make_state(bad_dma, bad_ds, bad_gs, -1);
  component_reads = 0;
  if (gba_load_state(state) || component_reads != 0)
  {
    fprintf(stderr, "FAIL %s was not rejected before component reads\n", label);
    return 1;
  }
  printf("PASS %s rejected before any component read\n", label);
  return 0;
}

static int rejects_dma_range(const char *label, int field)
{
  make_state(-1, -1, -1, field);
  component_reads = 0;
  if (gba_load_state(state) || component_reads != 0)
  {
    fprintf(stderr, "FAIL %s was not rejected before component reads\n", label);
    return 1;
  }
  printf("PASS %s rejected before any component read\n", label);
  return 0;
}

static int rejects_dma_field(const char *label, const char *field,
                             unsigned channel, u32 value)
{
  const u8 *doc;
  char channel_name[2] = {(char)('0' + channel), 0};
  u8 *stored;
  make_state(-1, -1, -1, -1);
  doc = bson_find_key(state, "dma");
  assert(doc);
  doc = bson_find_key(doc, channel_name);
  assert(doc);
  stored = (u8 *)bson_find_key(doc, field);
  assert(stored);
  put_u32(stored, value);
  component_reads = 0;
  if (gba_load_state(state) || component_reads != 0)
  {
    fprintf(stderr, "FAIL %s was not rejected before component reads\n", label);
    return 1;
  }
  printf("PASS %s rejected before any component read\n", label);
  return 0;
}

static int rejects_backup_range(const char *label, const char *field,
                                u32 value)
{
  const u8 *backup;
  u8 *stored;
  make_state(-1, -1, -1, -1);
  backup = bson_find_key(state, "backup");
  stored = (u8 *)bson_find_key(backup, field);
  assert(stored);
  put_u32(stored, value);
  if (memory_check_savestate(state))
  {
    fprintf(stderr, "FAIL %s accepted by memory preflight\n", label);
    return 1;
  }
  printf("PASS %s rejected by memory preflight\n", label);
  return 0;
}

static int rejects_input_range(const char *label, const char *field,
                               u32 value)
{
  const u8 *input;
  u8 *stored;
  make_state(-1, -1, -1, -1);
  input = bson_find_key(state, "input");
  stored = (u8 *)bson_find_key(input, field);
  assert(stored);
  put_u32(stored, value);
  component_reads = 0;
  if (gba_load_state(state) || component_reads != 0)
  {
    fprintf(stderr, "FAIL %s was not rejected before component reads\n", label);
    return 1;
  }
  printf("PASS %s rejected before any component read\n", label);
  return 0;
}

static int rejects_sound_range(const char *label, const char *channel,
                               const char *field, u32 value)
{
  const u8 *doc = NULL;
  u8 *stored;
  make_state(-1, -1, -1, -1);
  doc = bson_find_key(state, "sound");
  assert(doc);
  if (channel)
  {
    doc = bson_find_key(doc, channel);
    assert(doc);
  }
  stored = (u8 *)bson_find_key(doc, field);
  assert(stored);
  put_u32(stored, value);
  component_reads = 0;
  if (gba_load_state(state) || component_reads != 0)
  {
    fprintf(stderr, "FAIL %s was not rejected before component reads\n", label);
    return 1;
  }
  printf("PASS %s rejected before any component read\n", label);
  return 0;
}

static int rejects_rtc_output_without_bits(void)
{
  const u8 *backup;
  u8 *state_field, *bits_field;
  make_state(-1, -1, -1, -1);
  backup = bson_find_key(state, "backup");
  state_field = (u8 *)bson_find_key(backup, "rtc-state");
  bits_field = (u8 *)bson_find_key(backup, "rtc-data-bit-cnt");
  assert(state_field && bits_field);
  put_u32(state_field, 3); /* RTC_OUTPUT_DATA */
  put_u32(bits_field, 0);
  if (memory_check_savestate(state))
  {
    fputs("FAIL RTC output state without bits accepted\n", stderr);
    return 1;
  }
  puts("PASS RTC output state without bits rejected");
  return 0;
}

static int rejects_memory_bin_size(void)
{
  const u8 *memdoc;
  u8 *iwram_size;
  make_state(-1, -1, -1, -1);
  memdoc = bson_find_key(state, "memory");
  iwram_size = (u8 *)bson_find_key(memdoc, "iwram");
  assert(iwram_size);
  put_u32(iwram_size, 0x7FFF);
  component_reads = 0;
  if (gba_load_state(state) || component_reads != 0)
  {
    fputs("FAIL short IWRAM blob was not rejected before component reads\n", stderr);
    return 1;
  }
  puts("PASS short memory blob rejected before any component read");
  return 0;
}

int main(void)
{
  int channel;
  int failures = 0;
  char label[80];
  const u8 *timers_doc, *timer0;
  u8 *prescale_value, *status_value, *frequency_step_value;

  make_state(-1, -1, -1, -1);
  timers_doc = bson_find_key(state, "timers");
  timer0 = bson_find_key(timers_doc, "0");
  prescale_value = (u8 *)bson_find_key(timer0, "prescale");
  status_value = (u8 *)bson_find_key(timer0, "status");
  frequency_step_value = (u8 *)bson_find_key(timer0, "freq-step");
  assert(timers_doc && timer0 && prescale_value && status_value &&
         frequency_step_value);
  if (!main_check_savestate(state))
  {
    fputs("FAIL valid timer state rejected\n", stderr);
    failures++;
  }
  put_u32(prescale_value, 32);
  if (main_check_savestate(state))
  {
    fputs("FAIL invalid timer shift accepted\n", stderr);
    failures++;
  }
  else
    puts("PASS timer savestate rejects invalid shift count");
  put_u32(prescale_value, 0);
  put_u32(status_value, TIMER_CASCADE + 1);
  if (main_check_savestate(state))
  {
    fputs("FAIL invalid timer mode accepted\n", stderr);
    failures++;
  }
  else
    puts("PASS timer savestate rejects invalid mode selector");
  put_u32(status_value, TIMER_INACTIVE);
  put_u32(frequency_step_value, 0xFF000001u);
  if (main_check_savestate(state))
  {
    fputs("FAIL oversized timer frequency step accepted\n", stderr);
    failures++;
  }
  else
    puts("PASS timer savestate rejects oversized audio step");

  for (channel = 0; channel < DMA_CHAN_CNT; channel++)
  {
    snprintf(label, sizeof(label), "DMA channel %d missing irq", channel);
    failures += rejects_late_channel(label, channel, -1, -1);
  }
  for (channel = 0; channel < 2; channel++)
  {
    snprintf(label, sizeof(label), "direct-sound channel %d missing buf-idx", channel);
    failures += rejects_late_channel(label, -1, channel, -1);
  }
  for (channel = 0; channel < 4; channel++)
  {
    snprintf(label, sizeof(label), "PSG channel %d missing sample-tbl", channel);
    failures += rejects_late_channel(label, -1, -1, channel);
  }
  failures += rejects_dma_range("DMA source direction out of range", 2);
  failures += rejects_dma_range("DMA destination direction out of range", 3);
  failures += rejects_dma_range("DMA transfer size out of range", 5);
  failures += rejects_dma_field("DMA length exceeds channel 0 limit", "len", 0, 0x4001);
  failures += rejects_dma_field("DMA length exceeds channel 3 limit", "len", 3, 0x10001);
  failures += rejects_dma_field("DMA start mode out of range", "start", 0, 5);
  failures += rejects_dma_field("DMA repeat flag out of range", "repeat", 0, 2);
  failures += rejects_dma_field("DMA sound channel out of range", "dsc", 1, 3);
  failures += rejects_dma_field("DMA IRQ flag out of range", "irq", 0, 2);
  failures += rejects_backup_range("backup type out of range", "backup-type", 4);
  failures += rejects_backup_range("flash bank out of range", "flash-bank-num", 2);
  failures += rejects_backup_range("EEPROM address out of range", "eeprom-addr", 0xFFFFFFFFu);
  failures += rejects_backup_range("EEPROM base counter out of range", "eeprom-counter", 2);
  failures += rejects_backup_range("RTC data bit count out of range", "rtc-data-bit-cnt", 0xFFFFFFFFu);
  failures += rejects_rtc_output_without_bits();
  failures += rejects_memory_bin_size();
  failures += rejects_input_range("GBP countdown out of range", "gbp-frames", 0xFFFFFFFFu);
  failures += rejects_input_range("input key mask out of range", "prevkey", 0xFFFFFFFFu);
  failures += rejects_sound_range("direct-sound FIFO index out of range", "ds0", "fifo-base", 32);
  failures += rejects_sound_range("direct-sound fractional field out of range", "ds1", "fifo-frac", 0x01000000u);
  failures += rejects_sound_range("direct-sound buffer index out of range", "ds0", "buf-idx", BUFFER_SIZE);
  failures += rejects_sound_range("direct-sound shift count out of range", "ds0", "volume", 32);
  failures += rejects_sound_range("PSG square pattern index out of range", "gs0", "sample-tbl", 4);
  failures += rejects_sound_range("PSG envelope table index out of range", "gs0", "env-vol", 16);
  failures += rejects_sound_range("PSG sweep shift out of range", "gs0", "sweep-shift", 32);
  failures += rejects_sound_range("global sound buffer index out of range", NULL, "gbc-buf-idx", 0xFFFFFFFFu);

  make_state(-1, -1, -1, -1);
  if (!memory_check_savestate(state))
  {
    fprintf(stderr, "FAIL valid DMA fields rejected by memory preflight\n");
    failures++;
  }
  else
    puts("PASS valid DMA fields pass memory preflight");
  return failures ? 1 : 0;
}

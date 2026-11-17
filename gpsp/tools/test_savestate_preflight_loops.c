#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../common.h"
#include "../main.h"
#include "../savestate.h"
#include "../gba_memory.h"
#include "../sound.h"

static u8 state[GBA_STATE_MEM_SIZE];
static unsigned component_reads;

static void put_u32(u8 *p, u32 value)
{
  p[0] = (u8)value;
  p[1] = (u8)(value >> 8);
  p[2] = (u8)(value >> 16);
  p[3] = (u8)(value >> 24);
}

/* Actual input/main/memory/sound writers provide a valid BSON tree and
 * legal defaults. CPU execution is outside this test; only its writer/check
 * are stubbed. Readers are instrumented below to prove preflight atomicity. */
static void hide_key(const u8 *doc, const char *key)
{
  u8 *p = (u8 *)bson_find_key(doc, key);
  assert(p); p[-(int)strlen(key)-1] = '!';
}
static void make_state(int bad_dma, int bad_ds, int bad_gs, int bad_dma_field)
{
  static const char *const fields[] = {
    "src-addr", "dst-addr", "src-dir", "dst-dir", "len", "size",
    "repeat", "start", "dsc", "irq"
  };
  char key[4];
  const u8 *doc;
  extern u32 flash_bank_cnt;
  init_main(); init_sound();
  flash_bank_cnt = FLASH_SIZE_64KB; /* load_gamepak default */
  memset(state, 0, sizeof(state));
  gba_save_state(state);
  /* An accepted positive control must precede every negative mutation. */
  component_reads = 0;
  assert(gba_load_state(state)); assert(component_reads == 5);
  if (bad_dma >= 0) {
    key[0] = '0' + bad_dma; key[1] = 0;
    doc = bson_find_key(bson_find_key(state, "dma"), key);
    hide_key(doc, "irq");
  }
  if (bad_ds >= 0 || bad_gs >= 0) {
    key[0] = bad_ds >= 0 ? 'd' : 'g'; key[1] = 's';
    key[2] = '0' + (bad_ds >= 0 ? bad_ds : bad_gs); key[3] = 0;
    doc = bson_find_key(bson_find_key(state, "sound"), key);
    hide_key(doc, bad_ds >= 0 ? "buf-idx" : "sample-tbl");
  }
  if (bad_dma_field >= 0) {
    doc = bson_find_key(bson_find_key(state, "dma"), "0");
    put_u32((u8 *)bson_find_key(doc, fields[bad_dma_field]), 0xffffffffu);
  }
}

/* These components are irrelevant to the malformed later-channel cases;
 * counting their read paths proves gba_load_state rejects before mutation. */
unsigned cpu_write_savestate(u8 *dst) {
  u8 *start = dst, *header;
  bson_start_document(dst, "cpu", header);
  bson_finish_document(dst, header);
  return (unsigned)(dst-start);
}
bool cpu_check_savestate(const u8 *src) { (void)src; return true; }
bool cpu_read_savestate(const u8 *src) { (void)src; component_reads++; return true; }
bool input_read_savestate(const u8 *src) { (void)src; component_reads++; return true; }
bool main_read_savestate(const u8 *src) { (void)src; component_reads++; return true; }
bool memory_read_savestate(const u8 *src) { (void)src; component_reads++; return true; }
bool sound_read_savestate(const u8 *src) { (void)src; component_reads++; return true; }

u16 palette_ram[512], palette_ram_converted[512], io_registers[512], oam_ram[512];
u8 vram[1024 * 96];
u8 ewram[1024 * 256 * 2], iwram[1024 * 32 * 2];
u32 serial_get_irq_cycles(void) { return 0; }
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

/* A state saved with the 64K-entry ring (4.6.0-beta.4 and older): indices
 * up to SOUND_STATE_INDEX_RANGE pass the preflight (sound_read_savestate
 * masks them into the ring). */
static int accepts_old_ring_indices(void)
{
  const u8 *doc;
  const u32 lap = SOUND_STATE_INDEX_RANGE - BUFFER_SIZE;  /* a multiple of it */
  make_state(-1, -1, -1, -1);
  doc = bson_find_key(state, "sound");
  /* read position 1000, mixer 1000 + 1200, ds0 32 ahead, ds1 6 behind */
  put_u32((u8 *)bson_find_key(doc, "buf-base"), lap + 1000);
  put_u32((u8 *)bson_find_key(doc, "gbc-buf-idx"), lap + 2200);
  put_u32((u8 *)bson_find_key(bson_find_key(doc, "ds0"), "buf-idx"), lap + 2232);
  put_u32((u8 *)bson_find_key(bson_find_key(doc, "ds1"), "buf-idx"), lap + 2194);
  component_reads = 0;
  if (!gba_load_state(state) || component_reads != 5)
  {
    fprintf(stderr, "FAIL a state with 64K-ring indices was rejected\n");
    return 1;
  }
  /* (the masking itself: sound_timer_step, check_old_ring_indices) */
  puts("PASS a state with 64K-ring indices passes preflight");
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

/* Re-tag a document as an opaque binary element of identical total size.
 * Structural BSON remains valid: 4-byte binary size + subtype + data ==
 * the original document span. Untyped component lookups used to descend
 * into this binary body as though it had been recursively validated. */
static void binary_document(const u8 *parent, const char *key)
{
  u8 *value = (u8 *)bson_find_key(parent, key);
  assert(value); value[-(int)strlen(key)-2] = BSON_TYPE_BIN;
  put_u32(value, bson_read_u32(value)-5);
}
static void expect_preflight_rejection(void)
{
  component_reads = 0;
#ifdef EXPECT_UNSAFE_TYPES
  assert(gba_load_state(state)); assert(component_reads == 5);
#else
  assert(!gba_load_state(state)); assert(component_reads == 0);
#endif
}
static void document_types(void)
{
  static const char *const roots[] = {
    "cpu", "input", "emu", "timers", "memory", "backup", "dma", "sound"
  };
  unsigned i;
  for(i=0;i<sizeof(roots)/sizeof(roots[0]);i++) {
    make_state(-1,-1,-1,-1);binary_document(state,roots[i]);
    expect_preflight_rejection();
  }
  for(i=0;i<4;i++) {
    char key[2]={'0'+i,0}, channel[4]={'g','s','0'+i,0};
    make_state(-1,-1,-1,-1);binary_document(bson_find_key(state,"timers"),key);expect_preflight_rejection();
    make_state(-1,-1,-1,-1);binary_document(bson_find_key(state,"dma"),key);expect_preflight_rejection();
    make_state(-1,-1,-1,-1);binary_document(bson_find_key(state,"sound"),channel);expect_preflight_rejection();
    if(i<2){channel[0]='d';make_state(-1,-1,-1,-1);binary_document(bson_find_key(state,"sound"),channel);expect_preflight_rejection();}
  }
#ifdef EXPECT_UNSAFE_TYPES
  puts("BASELINE accepts all 22 opaque binary component/channel documents");
#else
  puts("PASS 22 opaque binary component/channel documents rejected before reads");
#endif
}
static void optional_types(void)
{
  static const char *const keys[]={"frame-count","serial-irq-cycles","rand-state","gbp-state"};
  unsigned i;
  for(i=0;i<4;i++) {
    u8 *parent,*value,*padding;u32 padsize;
    make_state(-1,-1,-1,-1);
    parent=(u8 *)bson_find_key(state,"emu");value=(u8 *)bson_find_key(parent,keys[i]);
    padding=(u8 *)bson_find_key(state,"zpadding");padsize=bson_read_u32(padding);
    /* Replace the four-byte scalar with a five-byte empty BIN. Grow its
     * parent by one and shrink the writer's padding by one, keeping the
     * fixed outer size and every later field correctly framed. */
    memmove(value+5,value+4,(size_t)(state+sizeof(state)-1-(value+5)));
    value[-(int)strlen(keys[i])-2]=BSON_TYPE_BIN;put_u32(value,0);value[4]=0;
    put_u32(parent,bson_read_u32(parent)+1);put_u32(padding+1,padsize-1);
    expect_preflight_rejection();
  }
  make_state(-1,-1,-1,-1);
  {
    u8 *v=(u8 *)bson_find_key(bson_find_key(state,"backup"),"rtc-base-time");
    v[-(int)strlen("rtc-base-time")-2]=BSON_TYPE_DOC;
    expect_preflight_rejection();
  }
#ifdef EXPECT_UNSAFE_TYPES
  puts("BASELINE accepts wrong optional scalar/RTC array types");
#else
  puts("PASS wrong optional scalar/RTC array types rejected before reads");
#endif
  make_state(-1,-1,-1,-1);
  for(i=0;i<4;i++)hide_key(bson_find_key(state,"emu"),keys[i]);
  hide_key(bson_find_key(state,"backup"),"rtc-base-time");
  hide_key(bson_find_key(state,"input"),"turbo-a");
  hide_key(bson_find_key(state,"input"),"turbo-b");
  hide_key(bson_find_key(state,"input"),"gbp-frames");
  hide_key(bson_find_key(state,"input"),"gbp-sent");
  component_reads=0;assert(gba_load_state(state));assert(component_reads==5);
  puts("PASS legacy optional-field omissions accepted");
}

int main(int argc, char **argv)
{
  int channel;
  int failures = 0;
  char label[80];
  const u8 *timers_doc, *timer0;
  u8 *prescale_value, *status_value, *frequency_step_value;

  document_types();optional_types();
  if(argc>1 && !strcmp(argv[1],"types")) return 0;
#ifdef REWIND
  {
    extern u32 gba_rewind_light_restore, gba_rewind_skip_arrays;
    make_state(-1,-1,-1,-1);
    gba_rewind_light_restore=1;gba_rewind_skip_arrays=1;
    gba_save_state(state);component_reads=0;
    assert(gba_load_state(state));assert(component_reads==5);
    gba_rewind_light_restore=gba_rewind_skip_arrays=0;
    puts("PASS trusted light rewind writer layout still loads");
  }
#endif
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
  failures += rejects_backup_range("backup type out of range", "backup-type", BACKUP_NONE + 1);
  failures += rejects_backup_range("flash bank out of range", "flash-bank-num", 2);
  failures += rejects_backup_range("EEPROM address out of range", "eeprom-addr", 0xFFFFFFFFu);
  failures += rejects_backup_range("EEPROM base counter out of range", "eeprom-counter", 2);
  failures += rejects_backup_range("RTC data bit count out of range", "rtc-data-bit-cnt", 0xFFFFFFFFu);
  failures += rejects_rtc_output_without_bits();
  failures += rejects_memory_bin_size();
  failures += rejects_input_range("GBP countdown out of range", "gbp-frames", 0xFFFFFFFFu);
  failures += rejects_input_range("input key mask out of range", "prevkey", 0xFFFFFFFFu);
  failures += rejects_sound_range("direct-sound FIFO index out of range", "ds0", "fifo-base", 32);
  /* fifo-frac is a supported multi-sample phase, not a [0,1) fraction. */
  make_state(-1, -1, -1, -1);
  put_u32((u8 *)bson_find_key(bson_find_key(bson_find_key(state, "sound"), "ds1"), "fifo-frac"), 0x01000000u);
  component_reads = 0;
  assert(gba_load_state(state)); assert(component_reads == 5);
  puts("PASS supported direct-sound phase accepted");
  failures += rejects_sound_range("direct-sound buffer index out of range", "ds0", "buf-idx", SOUND_STATE_INDEX_RANGE);
  failures += rejects_sound_range("odd direct-sound buffer index", "ds0", "buf-idx", 3);
  failures += rejects_sound_range("read position out of range", NULL, "buf-base", SOUND_STATE_INDEX_RANGE);
  failures += accepts_old_ring_indices();
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

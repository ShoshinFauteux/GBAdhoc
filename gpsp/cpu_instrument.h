
// Instrumentation and debugging code
// Used to count instruction and register usage
// Also provides some tracing capabilities


#ifdef MEMORY_STATS_ANALYZE
  // Collects memory stats by region, access type, etc.

  u32 memory_region_access_read_u8[16];
  u32 memory_region_access_read_s8[16];
  u32 memory_region_access_read_u16[16];
  u32 memory_region_access_read_s16[16];
  u32 memory_region_access_read_u32[16];
  u32 memory_region_access_write_u8[16];
  u32 memory_region_access_write_u16[16];
  u32 memory_region_access_write_u32[16];

  #define STATS_MEMORY_ACCESS(op, size, region) \
    memory_region_access_##op##_##size[region]++;

#else

  #define STATS_MEMORY_ACCESS(write, u32, region)

#endif

#ifdef REGISTER_USAGE_ANALYZE

  u64 instructions_total = 0;
  u64 arm_reg_freq[16];
  u64 arm_reg_access_total = 0;
  u64 arm_instructions_total = 0;
  u64 thumb_reg_freq[16];
  u64 thumb_reg_access_total = 0;
  u64 thumb_instructions_total = 0;

  // mla/long mla's addition operand are not counted yet.

  #define using_register(instruction_set, register, type)                     \
    instruction_set##_reg_freq[register]++;                                   \
    instruction_set##_reg_access_total++                                      \

  #define using_register_list(instruction_set, rlist, count)                  \
  {                                                                           \
    u32 i;                                                                    \
    for(i = 0; i < count; i++)                                                \
    {                                                                         \
      if((reg_list >> i) & 0x01)                                              \
      {                                                                       \
        using_register(instruction_set, i, memory_target);                    \
      }                                                                       \
    }                                                                         \
  }                                                                           \

  #define using_instruction(instruction_set)                                  \
    instruction_set##_instructions_total++;                                   \
    instructions_total++                                                      \

  int sort_tagged_element(const void *_a, const void *_b)
  {
    const u64 *a = _a;
    const u64 *b = _b;

    return (int)(b[1] - a[1]);
  }

  void print_register_usage(void)
  {
    u32 i;
    u64 arm_reg_freq_tagged[32];
    u64 thumb_reg_freq_tagged[32];
    double percent;
    double percent_total = 0.0;

    for(i = 0; i < 16; i++)
    {
      arm_reg_freq_tagged[i * 2] = i;
      arm_reg_freq_tagged[(i * 2) + 1] = arm_reg_freq[i];
      thumb_reg_freq_tagged[i * 2] = i;
      thumb_reg_freq_tagged[(i * 2) + 1] = thumb_reg_freq[i];
    }

    qsort(arm_reg_freq_tagged, 16, sizeof(u64) * 2, sort_tagged_element);
    qsort(thumb_reg_freq_tagged, 16, sizeof(u64) * 2, sort_tagged_element);

    printf("ARM register usage (%lf%% ARM instructions):\n",
     (arm_instructions_total * 100.0) / instructions_total);
    for(i = 0; i < 16; i++)
    {
      percent = (arm_reg_freq_tagged[(i * 2) + 1] * 100.0) /
       arm_reg_access_total;
      percent_total += percent;
      printf("r%02d: %lf%% (-- %lf%%)\n",
       (u32)arm_reg_freq_tagged[(i * 2)], percent, percent_total);
    }

    percent_total = 0.0;

    printf("\nThumb register usage (%lf%% Thumb instructions):\n",
     (thumb_instructions_total * 100.0) / instructions_total);
    for(i = 0; i < 16; i++)
    {
      percent = (thumb_reg_freq_tagged[(i * 2) + 1] * 100.0) /
       thumb_reg_access_total;
      percent_total += percent;
      printf("r%02d: %lf%% (-- %lf%%)\n",
       (u32)thumb_reg_freq_tagged[(i * 2)], percent, percent_total);
    }

    memset(arm_reg_freq, 0, sizeof(u64) * 16);
    memset(thumb_reg_freq, 0, sizeof(u64) * 16);
    arm_reg_access_total = 0;
    thumb_reg_access_total = 0;
  }

#else  /* REGISTER_USAGE_ANALYZE */

  #define using_register(instruction_set, register, type)
  #define using_register_list(instruction_set, rlist, count)
  #define using_instruction(instruction_set)

#endif  /* REGISTER_USAGE_ANALYZE */



#ifdef PC_RING
  /* DIAGNOSTIC (dr2-nfsu, twin only): ring of the last executed interpreter
   * instructions, dumped once when PC_RING_PC is entered with r1 negative. */
  #include <stdlib.h>
  #define PCR_N 262144
  static u32 pcr_pc[PCR_N], pcr_op[PCR_N], pcr_r[PCR_N][5];
  static u32 pcr_i, pcr_watch = 1, pcr_done;
  static void pc_ring_note(u32 pc, u32 mode, u32 op)
  {
    u32 k = pcr_i++ & (PCR_N - 1);
    pcr_pc[k] = pc | (mode ? 0 : 1); pcr_op[k] = op;
    pcr_r[k][0] = reg[0]; pcr_r[k][1] = reg[1]; pcr_r[k][2] = reg[2];
    pcr_r[k][3] = reg[3]; pcr_r[k][4] = reg[10];
    if (pcr_watch == 1) { const char *e = getenv("PC_RING_PC");
      pcr_watch = e ? strtoul(e, NULL, 16) : 0; }
    if (!pcr_done && pc == pcr_watch && (reg[1] & 0x80000000)) {
      u32 j; FILE *f = fopen(getenv("PC_RING_FILE") ? getenv("PC_RING_FILE") : "ring.txt", "w");
      pcr_done = 1;
      if (!f) return;
      for (j = 0; j < PCR_N; j++) {
        u32 q = (pcr_i + j) & (PCR_N - 1);
        fprintf(f, "%08x %08x r0=%08x r1=%08x r2=%08x r3=%08x r10=%08x\n",
          (unsigned)pcr_pc[q], (unsigned)pcr_op[q], (unsigned)pcr_r[q][0],
          (unsigned)pcr_r[q][1], (unsigned)pcr_r[q][2], (unsigned)pcr_r[q][3],
          (unsigned)pcr_r[q][4]);
      }
      fclose(f);
    }
  }
#endif

#ifdef TRACE_INSTRUCTIONS
  static void interp_trace_instruction(u32 pc, u32 mode)
  {
    if (mode)
      printf("Executed arm %x\n", pc);
    else
      printf("Executed thumb %x\n", pc);
    #ifdef TRACE_REGISTERS
    print_regs();
    #endif
  }
#endif


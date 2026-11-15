
// Instrumentation and debugging code
// Used to count instruction and register usage
// Also provides some tracing capabilities


/* The interpreter's (cpu.cc) memory-access and register-usage hooks.
 * Upstream gpSP counted them under MEMORY_STATS_ANALYZE and
 * REGISTER_USAGE_ANALYZE; those counters are gone (health A-2) and the call
 * sites stay, compiling to nothing. */
#define STATS_MEMORY_ACCESS(op, size, region)
#define using_register(instruction_set, register, type)
#define using_register_list(instruction_set, rlist, count)
#define using_instruction(instruction_set)



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


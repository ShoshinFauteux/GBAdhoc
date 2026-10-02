#ifndef ICPROBE_H
#define ICPROBE_H
/* The I-cache geometry probe (psp/icprobe.c, harness `icache_probe = 1`).
 * Logs icprobe_* events; 0 on success. */
int icprobe_run(void);
/* The kernel ranged-invalidate probe (harness `icache_inval_probe = 1`,
 * docs/JIT-COHERENCY.md).  Logs icinv_* events; 0 on success. */
int icinv_probe_run(void);
#endif

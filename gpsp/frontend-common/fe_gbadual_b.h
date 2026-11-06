/* fe_gbadual_b.h -- the section markers of GBA_LINK instance B's static state
 * (psp/gbalink_core_b.ld, docs/GBA-LINK-MIRROR.md 19).
 *
 * Weak: only the PSP's scripted partial link defines them; anywhere else they
 * resolve to NULL and fe_gbadual.c skips what they bound.  One list, included
 * by fe_gbadual.c and by tools/tests/test_gbadual_clock.c, so a new marker
 * cannot leave the host test declaring a different set. */
#ifndef FE_GBADUAL_B_H
#define FE_GBADUAL_B_H

extern char gbab_bdata_start[] __attribute__((weak));
extern char gbab_bdata_end[] __attribute__((weak));
extern char gbab_bdata_pristine[] __attribute__((weak));
extern char gbab_bregion_start[] __attribute__((weak));
extern char gbab_bregion_lend[] __attribute__((weak));
extern char gbab_bsdata_start[] __attribute__((weak));
extern char gbab_bsdata_end[] __attribute__((weak));
extern char gbab_bsbss_start[] __attribute__((weak));
extern char gbab_bsbss_end[] __attribute__((weak));

#endif

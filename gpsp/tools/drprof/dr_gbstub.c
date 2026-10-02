/* dr_gbstub.c -- the GB dual-link machine (gbcore_dual.c) is not part of the
 * dynarec twin: it needs two objcopy-renamed TGB Dual instances
 * (gbcore_instance.ld) and no GBA fixture ever starts a GB link.  fe_host.c
 * and fe_gblink.c still reference it, so the twin links these inert stubs:
 * creation fails, which fe_gblink already treats as "link unavailable". */
#include <stddef.h>
#include "gbcore_dual.h"

gbdual_t *gbdual_create(const gbdual_config_t *config) { (void)config; return NULL; }
void gbdual_destroy(gbdual_t *dual) { (void)dual; }
int gbdual_advance(gbdual_t *dual, gbdual_input_fn input, void *userdata)
{ (void)dual; (void)input; (void)userdata; return -1; }
uint64_t gbdual_frame(const gbdual_t *dual, int slot) { (void)dual; (void)slot; return 0; }
uint64_t gbdual_lines(const gbdual_t *dual) { (void)dual; return 0; }
uint64_t gbdual_serial_bytes(const gbdual_t *dual, int slot) { (void)dual; (void)slot; return 0; }
gbcore_t *gbdual_core(const gbdual_t *dual, int slot) { (void)dual; (void)slot; return NULL; }
const gbcore_api_t *gbdual_api(int slot) { (void)slot; return NULL; }
uint64_t gbdual_sync_hash(gbdual_t *dual) { (void)dual; return 0; }
